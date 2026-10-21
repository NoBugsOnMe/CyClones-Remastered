"""Reader for CyClones 'BOL' lump archives (CYCLONE.HD, CYCLONE.CD, PIC.BOL).

Layout (little-endian), reverse-engineered from the 1994 CD release:
  header (16 bytes):
    u16 lump_count
    u32 dir_offset      absolute file offset of the directory
    u32 dir_size        bytes in the directory (entries + name table)
    6 bytes padding
  directory at dir_offset:
    lump_count x { u32 offset, u32 size, u32 name_offset }   (name_offset is relative to dir_offset)
    followed by NUL-terminated lump names
  Zero-size lumps are section markers (startflats/endflats, startsprites/endsprites, ...).
"""
import struct
import sys


class Bol:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        count, dir_off, dir_size = struct.unpack_from('<HII', self.data, 0)
        if dir_off + dir_size != len(self.data):
            raise ValueError(f'{path}: directory does not end at EOF')
        self.lumps = []
        for i in range(count):
            off, size, name_off = struct.unpack_from('<III', self.data, dir_off + i * 12)
            p = dir_off + name_off
            end = self.data.index(b'\x00', p)
            name = self.data[p:end].decode('latin-1')
            self.lumps.append((name, off, size))

    def get(self, name):
        for n, off, size in self.lumps:
            if n.lower() == name.lower():
                return self.data[off:off + size]
        raise KeyError(name)

    def sections(self):
        """Yield (section, name, off, size) using start*/end* markers."""
        section = ''
        for n, off, size in self.lumps:
            if size == 0 and n.startswith('start'):
                section = n[5:]
                continue
            if size == 0 and n.startswith('end'):
                section = ''
                continue
            yield section, n, off, size


if __name__ == '__main__':
    b = Bol(sys.argv[1])
    for sec, n, off, size in b.sections():
        print(f'{sec:10} {n:16} {off:9} {size:8}')
