"""Convert a DOS/4GW-bound LE executable (Watcom) into a relocated ELF32 for analysis.

usage: python le2elf.py CYCLONE.EXE out.elf [map.txt]

- Finds the bound app's MZ header (the one whose e_lfanew points at an 'LE' header).
- Loads every object at its preferred base, applies internal fixups (types 2,3,5,6,7,8).
- Writes an ELF32 i386 executable with one PT_LOAD per object and the LE entry point.
"""
import struct
import sys


def find_le(d):
    i = -1
    while True:
        i = d.find(b'MZ', i + 1)
        if i < 0:
            raise SystemExit('no bound LE found')
        if i + 0x40 > len(d):
            continue
        lfa = struct.unpack_from('<I', d, i + 0x3C)[0]
        j = i + lfa
        if 0 < lfa < len(d) and d[j:j + 4] == b'LE\x00\x00':
            return i, j


def load(path):
    d = open(path, 'rb').read()
    mz, le = find_le(d)
    u32 = lambda o: struct.unpack_from('<I', d, le + o)[0]
    npages, eip_obj, eip = u32(0x14), u32(0x18), u32(0x1C)
    esp_obj, esp = u32(0x20), u32(0x24)
    psize, lastpage = u32(0x28), u32(0x2C)
    obj_off, nobj = u32(0x40), u32(0x44)
    opt_off = u32(0x48)
    fpt_off, frt_off = u32(0x68), u32(0x6C)
    data_off = mz + u32(0x80)

    objs = []
    for k in range(nobj):
        vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', d, le + obj_off + 24 * k)
        objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcnt=pcnt,
                         mem=bytearray(max(vsize, pcnt * psize))))

    # page table: 4 bytes per page, LE format = 24-bit big-endian page number + flags
    def page_file_offset(p):  # p is 1-based logical page
        e = d[le + opt_off + 4 * (p - 1): le + opt_off + 4 * p]
        num = (e[0] << 16) | (e[1] << 8) | e[2]
        return data_off + (num - 1) * psize, e[3]

    for o in objs:
        for k in range(o['pcnt']):
            p = o['pidx'] + k
            off, fl = page_file_offset(p)
            size = lastpage if p == npages else psize
            chunk = d[off:off + size]
            o['mem'][k * psize:k * psize + len(chunk)] = chunk

    # fixups
    fpt = [struct.unpack_from('<I', d, le + fpt_off + 4 * k)[0] for k in range(npages + 1)]
    stats = {}
    page_owner = {}
    for oi, o in enumerate(objs):
        for k in range(o['pcnt']):
            page_owner[o['pidx'] + k] = (oi, k)
    for p in range(1, npages + 1):
        if p not in page_owner:
            continue
        oi, k = page_owner[p]
        mem = objs[oi]['mem']
        page_base_off = k * psize
        page_base_addr = objs[oi]['base'] + page_base_off
        pos = le + frt_off + fpt[p - 1]
        end = le + frt_off + fpt[p]
        while pos < end:
            src, flg = d[pos], d[pos + 1]
            pos += 2
            stype = src & 0x0F
            if src & 0x20:
                cnt = d[pos]; pos += 1
                srcoffs = None
            else:
                srcoffs = [struct.unpack_from('<h', d, pos)[0]]; pos += 2
            ttype = flg & 0x03
            if ttype != 0:
                raise SystemExit(f'unsupported fixup target type {ttype} on page {p}')
            if flg & 0x40:
                tobj = struct.unpack_from('<H', d, pos)[0]; pos += 2
            else:
                tobj = d[pos]; pos += 1
            toff = 0
            if stype != 0x02:
                if flg & 0x10:
                    toff = struct.unpack_from('<I', d, pos)[0]; pos += 4
                else:
                    toff = struct.unpack_from('<H', d, pos)[0]; pos += 2
            if srcoffs is None:
                srcoffs = [struct.unpack_from('<h', d, pos + 2 * n)[0] for n in range(cnt)]
                pos += 2 * cnt
            target = objs[tobj - 1]['base'] + toff
            for so in srcoffs:
                at = page_base_off + so
                if at < 0 or at + 4 > len(mem) + 4:
                    continue
                stats[stype] = stats.get(stype, 0) + 1
                if stype == 0x07:
                    if at + 4 <= len(mem):
                        struct.pack_into('<I', mem, at, target & 0xFFFFFFFF)
                elif stype == 0x08:
                    if at + 4 <= len(mem):
                        struct.pack_into('<i', mem, at, target - (page_base_addr + so + 4))
                elif stype == 0x05:
                    struct.pack_into('<H', mem, at, target & 0xFFFF)
                elif stype == 0x06:
                    struct.pack_into('<IH', mem, at, target & 0xFFFFFFFF, tobj)
                elif stype == 0x02:
                    struct.pack_into('<H', mem, at, tobj)
                elif stype == 0x03:
                    struct.pack_into('<HH', mem, at, toff & 0xFFFF, tobj)
                else:
                    raise SystemExit(f'unsupported fixup source type {stype:#x}')
    entry = objs[eip_obj - 1]['base'] + eip
    return objs, entry, objs[esp_obj - 1]['base'] + esp, stats


def write_elf(objs, entry, path):
    phoff = 52
    phnum = len(objs)
    data_start = phoff + 32 * phnum
    data_start = (data_start + 0xFFF) & ~0xFFF
    out = bytearray()
    e_ident = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\x00' * 8
    out += e_ident + struct.pack('<HHIIIIIHHHHHH', 2, 3, 1, entry, phoff, 0, 0, 52, 32, phnum, 0, 0, 0)
    offs = []
    cur = data_start
    for o in objs:
        offs.append(cur)
        cur += (len(o['mem']) + 0xFFF) & ~0xFFF
    for o, off in zip(objs, offs):
        flags = 0
        if o['flags'] & 0x1: flags |= 4      # readable
        if o['flags'] & 0x2: flags |= 2      # writable
        if o['flags'] & 0x4: flags |= 1      # executable
        out += struct.pack('<IIIIIIII', 1, off, o['base'], o['base'], len(o['mem']), len(o['mem']), flags, 0x1000)
    out += b'\x00' * (data_start - len(out))
    for o, off in zip(objs, offs):
        out += b'\x00' * (off - len(out))
        out += o['mem']
    open(path, 'wb').write(out)


if __name__ == '__main__':
    objs, entry, stack, stats = load(sys.argv[1])
    for i, o in enumerate(objs):
        print(f"object {i+1}: base={o['base']:#x} vsize={o['vsize']:#x} flags={o['flags']:#x} pages={o['pcnt']}")
    print(f'entry={entry:#x} stack={stack:#x} fixups={stats}')
    write_elf(objs, entry, sys.argv[2])
    print('wrote', sys.argv[2])
