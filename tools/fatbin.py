#!/usr/bin/env python3
"""Extract CUDA fatbin entries from a PE/any file and report PTX targets/ISA and ELF archs.
Usage: fatbin.py <file> [outdir]   (extracts and inspects CUDA fatbins embedded in any file)
"""
import struct, sys, os, collections

def lz4_lenient(src, usz):
    """LZ4 block decoder that tolerates NVIDIA's end-of-block rule violations."""
    out = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]; i += 1; ll += b
                if b != 255: break
        out += src[i:i+ll]; i += ll
        if i >= n or len(out) >= usz: break
        off = src[i] | (src[i+1] << 8); i += 2
        ml = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        start = len(out) - off
        if off == 0 or start < 0: raise ValueError('bad offset')
        for k in range(ml): out.append(out[start + k])
    return bytes(out[:usz])


FB = b'\x50\xed\x55\xba'
KIND = {1: 'ptx', 2: 'elf'}

def entries(d):
    i = 0
    n = 0
    while True:
        i = d.find(FB, i)
        if i < 0:
            return
        magic, ver, hdr, size = struct.unpack_from('<IHHQ', d, i)
        if ver != 1 or hdr != 16:
            i += 4
            continue
        p = i + hdr
        end = p + size
        while p + 64 <= end:
            kind, v2, hsz, psz = struct.unpack_from('<HHIQ', d, p)
            if kind not in KIND or hsz < 64:
                break
            # fat_text_header: kind@0 ver@2 hdrsz@4 padded@8 unk@16 payload@20 unk@24 sm@28 bits@32 unk@36 flags@40 unk@48 decomp@56
            real = struct.unpack_from('<I', d, p + 16)[0]
            arch = struct.unpack_from('<I', d, p + 28)[0]
            flags = struct.unpack_from('<Q', d, p + 40)[0]
            usz = struct.unpack_from('<Q', d, p + 56)[0]
            payload = d[p + hsz:p + hsz + real]
            comp = bool(flags & 0x2000)
            if comp:
                try:
                    payload = lz4_lenient(payload, usz)
                    if len(payload) != usz: payload = None
                except Exception as e:
                    payload = None
            yield dict(fatbin=n, off=p, kind=KIND[kind], arch=arch, comp=comp, size=psz, data=payload)
            p += hsz + psz
        n += 1
        i = end

def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    d = open(path, 'rb').read()
    summ = collections.Counter()
    bad = 0
    if out:
        os.makedirs(out, exist_ok=True)
    for k, e in enumerate(entries(d)):
        summ[(e['kind'], e['arch'], e['comp'])] += 1
        if e['data'] is None:
            bad += 1
            continue
        if out:
            ext = 'ptx' if e['kind'] == 'ptx' else 'cubin'
            with open(f"{out}/fb{e['fatbin']:03d}_{k:03d}_sm{e['arch']}.{ext}", 'wb') as f:
                f.write(e['data'])
    for (kind, arch, comp), n in sorted(summ.items()):
        print(f"{kind} sm_{arch} {'lz4' if comp else 'raw'}: {n}")
    print(f"decompress failures: {bad}")

if __name__ == '__main__':
    main()
