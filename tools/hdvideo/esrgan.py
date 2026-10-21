#!/usr/bin/env python3
"""Real-ESRGAN SRVGGNetCompact (realesr-general-x4v3) without torch.

Loads the .pth zip with a minimal unpickler, builds the network as an ONNX
graph and runs it with ONNX Runtime on the CPU.

  esrgan.py build OUT.onnx [denoise]   (denoise 0..1 blends x4v3 with wdn-x4v3)
"""
import io, pickle, sys, zipfile
import numpy as np

MODELS = '/home/claude/hdvid/models'


class _Storage:
    def __init__(self, dtype):
        self.dtype = dtype


_DT = {'FloatStorage': np.float32, 'HalfStorage': np.float16, 'DoubleStorage': np.float64,
       'LongStorage': np.int64, 'IntStorage': np.int32}


def load_pth(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    root = names[0].split('/')[0]
    pkl = next(n for n in names if n.endswith('data.pkl'))

    def rebuild(storage, offset, size, stride, *rest):
        arr = storage
        n = int(np.prod(size)) if size else 1
        flat = arr[offset:offset + (max((s - 1) * st for s, st in zip(size, stride)) + 1 if size else 1)]
        if not size:
            return flat[0]
        return np.lib.stride_tricks.as_strided(flat, shape=size,
                                               strides=[s * arr.itemsize for s in stride]).copy()

    class U(pickle.Unpickler):
        def find_class(self, mod, name):
            if name == '_rebuild_tensor_v2' or name == '_rebuild_tensor':
                return rebuild
            if name in _DT:
                return _DT[name]
            if name == 'OrderedDict':
                import collections
                return collections.OrderedDict
            if mod.startswith('torch'):
                return lambda *a, **k: None
            return super().find_class(mod, name)

        def persistent_load(self, pid):
            _, dtype, key, loc, numel = pid
            raw = z.read(f'{root}/data/{key}')
            return np.frombuffer(raw, dtype=dtype)

    obj = U(io.BytesIO(z.read(pkl))).load()
    for k in ('params_ema', 'params'):
        if isinstance(obj, dict) and k in obj:
            return obj[k]
    return obj


def build(out, denoise=0.5):
    import onnx
    from onnx import helper, TensorProto, numpy_helper
    a = load_pth(f'{MODELS}/realesr-general-x4v3.pth')
    b = load_pth(f'{MODELS}/realesr-general-wdn-x4v3.pth')
    p = {k: (denoise * a[k] + (1 - denoise) * b[k]).astype(np.float32) for k in a}
    nodes, inits = [], []
    x = 'input'
    i = 0
    layer = 0
    keys = sorted({int(k.split('.')[1]) for k in p})
    for idx in keys:
        w = p[f'body.{idx}.weight']
        if w.ndim == 4:
            wn, bn = f'w{idx}', f'b{idx}'
            inits += [numpy_helper.from_array(w, wn), numpy_helper.from_array(p[f'body.{idx}.bias'], bn)]
            y = f'c{idx}'
            nodes.append(helper.make_node('Conv', [x, wn, bn], [y], pads=[1, 1, 1, 1], kernel_shape=[3, 3]))
            x = y
        else:  # PReLU, per-channel slope
            sn = f's{idx}'
            inits.append(numpy_helper.from_array(w.reshape(-1, 1, 1), sn))
            y = f'p{idx}'
            nodes.append(helper.make_node('PRelu', [x, sn], [y]))
            x = y
    nodes.append(helper.make_node('DepthToSpace', [x], ['ps'], blocksize=4, mode='CRD'))
    inits.append(numpy_helper.from_array(np.array([1, 1, 4, 4], np.float32), 'scales'))
    nodes.append(helper.make_node('Resize', ['input', '', 'scales'], ['base'], mode='nearest'))
    nodes.append(helper.make_node('Add', ['ps', 'base'], ['output']))
    g = helper.make_graph(nodes, 'srvgg',
                          [helper.make_tensor_value_info('input', TensorProto.FLOAT, [1, 3, 'h', 'w'])],
                          [helper.make_tensor_value_info('output', TensorProto.FLOAT, [1, 3, 'H', 'W'])], inits)
    m = helper.make_model(g, opset_imports=[helper.make_opsetid('', 17)], ir_version=9)
    onnx.checker.check_model(m)
    onnx.save(m, out)


class Upscaler:
    def __init__(self, onnx_path, threads=2):
        import onnxruntime as ort
        o = ort.SessionOptions()
        o.intra_op_num_threads = threads
        o.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.s = ort.InferenceSession(onnx_path, o, providers=['CPUExecutionProvider'])

    def __call__(self, rgb):
        """rgb: HxWx3 uint8 -> 4H x 4W x 3 float32 (0..1, unclipped)"""
        x = (rgb.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
        y = self.s.run(None, {'input': x})[0][0]
        return y.transpose(1, 2, 0)


if __name__ == '__main__':
    if sys.argv[1] == 'build':
        build(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.5)
