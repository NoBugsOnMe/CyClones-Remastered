"""Parser for CyClones entity chunks (MAP: OBJ/DOOR/ITEM/MON/TRIG + text; GLB: BMON/MBTX/GITX...).
Record layouts follow the field-by-field fread() sequences in CYCLONE.EXE:
  common header  FUN_00010010   script FUN_00039f07   OBJ FUN_0001029b   DOOR FUN_00010577
  ITEM FUN_000108dc   MON FUN_00010d05   BMON FUN_000115e2   text FUN_00010184   TRIG FUN_0003a1fa
"""
import struct, sys

class R:
    def __init__(s, d, p=0): s.d, s.p = d, p
    def u8(s): v = s.d[s.p]; s.p += 1; return v
    def s8(s): v = struct.unpack_from('<b', s.d, s.p)[0]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.d, s.p)[0]; s.p += 2; return v
    def s16(s): v = struct.unpack_from('<h', s.d, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.d, s.p)[0]; s.p += 4; return v
    def raw(s, n): v = s.d[s.p:s.p + n]; s.p += n; return v
    def name(s, n): return s.raw(n).split(b'\0')[0].decode('latin1')

def chunks(d):
    p, out = 0, []
    while p + 10 <= len(d):
        tag = d[p:p + 4].decode('latin1'); t, sz = struct.unpack_from('<HI', d, p + 4)
        out.append((tag, t, d[p + 10:p + 10 + sz])); p += 10 + sz
    return out

def script(r):
    s = dict(a=r.u16(), b=r.u32(), c=r.u32(), kind=r.u8(), next=r.u32(), d=r.u32(), e=r.raw(16).hex(), head=r.u32())
    acts = []
    if s['head']:
        while True:
            a = dict(op=r.u16(), arg=r.raw(20).hex(), next=r.u32()); acts.append(a)
            if not a['next']: break
    s['actions'] = acts
    s['head2'] = r.u32(); s['f'] = r.u32(); acts2 = []
    if s['head2']:
        while True:
            a = dict(op=r.u16(), arg=r.raw(20).hex(), next=r.u32(), prev=r.u32()); acts2.append(a)
            if not a['next']: break
    s['actions2'] = acts2
    return s

def header(r, ctype):
    h = dict(id=r.u16(), x=r.u16(), z=r.u16(), y=r.s16(), skill=r.u8(), d=r.u16(), e=r.u16(), f=r.u32(), g=r.u32())
    h['link'] = 0
    if ctype:
        h['link'] = r.u32()
        if h['link']: h['script'] = script(r)
    return h

def obj(r, ct):
    h = header(r, ct)
    h.update(kind=r.u16(), name=r.name(10), o25=r.u16(), o27=r.u16(), b=list(r.raw(7)), o30=r.u16(), tail=r.raw(19).hex())
    return h

def door(r, ct):
    h = header(r, ct)
    h.update(kind=r.u16(), tex=r.name(10), o25=r.u16(), o27=r.u16(), flags=r.u8(), o2a=r.u16(),
             b=list(r.raw(6)), o32=r.u16(), o34=r.u16(), tail=r.raw(10).hex())
    return h

def item(r, ct):
    h = header(r, ct)
    h.update(kind=r.u16(), i1b=r.u16(), name=r.name(10), i27=r.u16(), i2c=r.u16(), i2e=r.u16(), i31=r.u16(),
             i33=r.raw(40).hex(), i15=r.u16(), tail=r.raw(19).hex())
    return h

def mon(r, ct):
    h = header(r, ct)
    h.update(kind=r.u16(), m2=r.u16(), m20=r.raw(20).hex(), bytes19=list(r.raw(19)))
    links = struct.unpack('<8I', r.raw(32)); h['m8'] = r.raw(8).hex(); h['b6'] = list(r.raw(6)); h['tail'] = r.raw(97).hex()
    h['carried'] = [item(r, ct) for l in links if l]
    return h

def bmon(r, ct):
    m = dict(id=r.u16(), name=r.name(10), w0=r.u16(), b8=list(r.raw(8)))
    m['l6'] = struct.unpack('<6I', r.raw(24))           # local_60,5c,68,6c,70,64
    m['attP'] = struct.unpack('<3I', r.raw(12)); m['attQ'] = struct.unpack('<3I', r.raw(12))
    m['w432'] = r.u16(); m['w434'] = r.u16(); m['b0a4'] = r.u8(); m['l78'] = r.u32(); m['l74'] = r.u32()
    items = struct.unpack('<3I', r.raw(12)); m['x3'] = r.raw(3).hex(); m['w43d'] = r.u16(); m['b43f'] = r.u8()
    m['x27'] = r.raw(27).hex(); m['x4a'] = r.u32(); m['x4b'] = r.u32()
    m['items'] = [item(r, ct) for l in items if l]
    def seq():
        n = r.u8(); return list(r.raw(n))
    m['seqA'] = seq(); m['seqA_x'] = seq(); m['seqB'] = seq()
    if m['l6'][3]: m['seqB_x'] = seq()
    m['seqC'] = seq()                                   # l6[4] must be nonzero
    if m['l6'][5]: m['seqC_x'] = seq()
    for i in range(3):
        if m['attP'][i]: m['atk%d' % i] = seq()
    for i in range(3):
        if m['attQ'][i]: m['atk%d_x' % i] = seq()
    if m['l78']: m['seqD'] = seq()
    if m['l74']: m['seqD_x'] = seq()
    return m

def texts(data):
    r, out = R(data), {}
    while r.p < len(data):
        i = r.u16(); n = r.u16(); out[i] = r.raw(n).decode('latin1') if n else ''
    return out

def records(data, ct, fn):
    r, out = R(data), []
    while r.p < len(data): out.append(fn(r, ct))
    assert r.p == len(data), (r.p, len(data))
    return out

def trig(data):
    # The grid is indexed z * 64 + x, but the chains follow it column by column (x outer, z inner;
    # FUN_0003a1fa).
    r = R(data); grid = struct.unpack('<4096I', r.raw(16384)); out = []
    for x in range(64):
        for z in range(64):
            if not grid[z * 64 + x]: continue
            while True:
                s = script(r); s['cell'] = (x, z); out.append(s)
                if not s['next']: break
    assert r.p == len(data), (r.p, len(data))
    return out

def load_map(path):
    res = {}
    for tag, ct, data in chunks(open(path, 'rb').read()):
        if tag == 'OBJ ': res['obj'] = records(data, ct, obj)
        elif tag == 'DOOR': res['door'] = records(data, ct, door)
        elif tag == 'ITEM': res['item'] = records(data, ct, item)
        elif tag == 'MON ': res['mon'] = records(data, ct, mon)
        elif tag == 'TRIG': res['trig'] = trig(data)
        elif tag in ('OTXT', 'DTXT', 'ITXT', 'MTXT'): res[tag] = texts(data)
    return res

def load_glb(path):
    res = {}
    for tag, ct, data in chunks(open(path, 'rb').read()):
        if tag == 'BMON': res['bmon'] = records(data, ct, bmon)
        elif tag in ('GITX', 'GDTX', 'GMTX', 'MBTX', 'GOTX'): res[tag] = texts(data)
    return res

if __name__ == '__main__':
    import json
    p = sys.argv[1]
    res = load_glb(p) if p.upper().endswith('.GLB') else load_map(p)
    print(json.dumps(res, indent=1)[:int(sys.argv[2]) if len(sys.argv) > 2 else None])
