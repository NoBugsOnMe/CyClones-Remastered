"""CyClones level (.MAP) reader — chunk layout recovered from CYCLONE.EXE's loader (FUN_00014179).

File = sequence of chunks: char tag[4]; u16 type; u32 size; u8 data[size].

Grid chunks (64 x 64 cells, row-major, u16 layers = 8192 bytes, u8 layers = 4096 bytes):
  'MAP '  u16 width, u16 height (playable area, e.g. 40x40), then 5 u8 layers:
          info0 (bits0-2 -> flat rec+0x42, bits3-5 -> rec+0x45), info1 (global cell grid @0x832d4),
          info2..4 (3 bytes per cell @0x802d4)
  'MAPC'  ceiling: u16 tex (low 14 bits = flat index, top 2 bits flags), u16 vertex height, u8 extra
  'MAPF'  floor:   same layout
  'MAP1'  extra surface 1 (same layout)       'MAP2'  extra surface 2 (same layout)
  'MAPN'  north edge wall: u16 tex_front, u16 tex_back, u16 a, u8 flags, u8 b, u16 z_bottom, u16 z_top
  'MAPW'  west edge wall:  same layout
Heights of a surface are stored per vertex: corner (x,y) uses layer[y*64+x] (x+1 / y+1 for the others).
Wall texture value v -> lump 'startwalls' + v (v=1 is the first wall); flats likewise from 'startflats'.
"""
import struct

W = 64


def u16layer(b):
    return list(struct.unpack('<4096H', b))


def u8layer(b):
    return list(b)


class Surface:
    def __init__(self, data):
        self.tex = u16layer(data[0:8192])
        self.height = u16layer(data[8192:16384])
        self.extra = u8layer(data[16384:20480])


class Edges:
    def __init__(self, data):
        o = 0
        def take(n):
            nonlocal o
            r = data[o:o + n]; o += n; return r
        self.tex_front = u16layer(take(8192))
        self.tex_back = u16layer(take(8192))
        self.a = u16layer(take(8192))
        self.flags = u8layer(take(4096))
        self.b = u8layer(take(4096))
        self.z_bottom = u16layer(take(8192))
        self.z_top = u16layer(take(8192))


class CyMap:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.chunks = {}
        o = 0
        while o + 10 <= len(d):
            tag = d[o:o + 4].decode('latin-1')
            _typ, size = struct.unpack_from('<HI', d, o + 4)
            self.chunks[tag] = d[o + 10:o + 10 + size]
            o += 10 + size
        m = self.chunks['MAP ']
        self.width, self.height = struct.unpack_from('<HH', m, 0)
        self.info = [u8layer(m[4 + 4096 * k:4 + 4096 * (k + 1)]) for k in range(5)]
        self.ceil = Surface(self.chunks['MAPC'])
        self.floor = Surface(self.chunks['MAPF'])
        self.s1 = Surface(self.chunks['MAP1'])
        self.s2 = Surface(self.chunks['MAP2'])
        self.north = Edges(self.chunks['MAPN'])
        self.west = Edges(self.chunks['MAPW'])
        cube = self.chunks.get('CUBE', b'')
        self.cubes = [cube[i:i + 56] for i in range(0, len(cube) - 55, 56)]
