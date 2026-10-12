"""Minimal read-only MPQ (format 0/1) reader: hash/block tables, sector files, zlib and stored data.

Enough to read archives written by common modding tools (and the wow-retail-backport kit), not a general
replacement for StormLib: PKWARE implode, bzip2, LZMA, Huffman/ADPCM and patch files are refused with a
clear error rather than misread.
"""
import struct
import zlib

_CRYPT = None


def _crypt_table():
    global _CRYPT
    if _CRYPT is None:
        table, seed = [0] * 0x500, 0x00100001
        for i in range(0x100):
            idx = i
            for _ in range(5):
                seed = (seed * 125 + 3) % 0x2AAAAB
                hi = (seed & 0xFFFF) << 16
                seed = (seed * 125 + 3) % 0x2AAAAB
                table[idx] = hi | (seed & 0xFFFF)
                idx += 0x100
        _CRYPT = table
    return _CRYPT


def _hash(s, kind):
    t = _crypt_table()
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().replace('/', '\\').encode('latin-1'):
        s1 = (t[kind * 0x100 + ch] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1


def _decrypt(data, key):
    t = _crypt_table()
    out = bytearray(len(data) - len(data) % 4)
    seed = 0xEEEEEEEE
    for i in range(0, len(out), 4):
        seed = (seed + t[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        v = struct.unpack_from('<I', data, i)[0]
        d = (v ^ (key + seed)) & 0xFFFFFFFF
        key = ((((~key) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (d + seed + (seed << 5) + 3) & 0xFFFFFFFF
        struct.pack_into('<I', out, i, d)
    return bytes(out) + data[len(out):]


FLAG_IMPLODE, FLAG_COMPRESS, FLAG_ENCRYPTED = 0x100, 0x200, 0x10000
FLAG_SINGLE_UNIT, FLAG_EXISTS = 0x1000000, 0x80000000


class MpqReader:
    def __init__(self, path):
        self._f = open(path, 'rb')
        head = self._f.read(4096)
        off = head.find(b'MPQ\x1a')
        if off < 0:
            raise ValueError(f'{path}: not an MPQ archive')
        self._base = off
        (_, hsize, _, fmt, sshift, hpos, bpos, hcount, bcount) = struct.unpack_from('<4sIIHHIIII', head, off)
        self._sector = 512 << sshift
        f = self._f
        f.seek(off + hpos)
        ht = _decrypt(f.read(hcount * 16), _hash('(hash table)', 3))
        f.seek(off + bpos)
        bt = _decrypt(f.read(bcount * 16), _hash('(block table)', 3))
        self._hashes = {}
        for i in range(hcount):
            a, b, _loc, _plat, blk = struct.unpack_from('<IIHHI', ht, i * 16)
            if blk < 0xFFFFFFFE:
                self._hashes[(a, b)] = blk
        self._blocks = [struct.unpack_from('<IIII', bt, i * 16) for i in range(bcount)]

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _block(self, name):
        blk = self._hashes.get((_hash(name, 1), _hash(name, 2)))
        if blk is None:
            return None
        return self._blocks[blk]

    def has(self, name):
        return self._block(name) is not None

    def read(self, name):
        b = self._block(name)
        if b is None:
            raise KeyError(name)
        pos, csize, size, flags = b
        if not flags & FLAG_EXISTS:
            raise KeyError(name)
        if flags & (FLAG_ENCRYPTED | FLAG_IMPLODE):
            raise NotImplementedError(f'{name}: encrypted or imploded files are not supported')
        self._f.seek(self._base + pos)
        raw = self._f.read(csize)
        if not flags & FLAG_COMPRESS:
            return raw[:size]
        if flags & FLAG_SINGLE_UNIT:
            return self._unpack(raw, size, name)
        nsec = (size + self._sector - 1) // self._sector
        offs = struct.unpack_from('<%dI' % (nsec + 1), raw, 0)
        out = bytearray()
        for i in range(nsec):
            want = min(self._sector, size - i * self._sector)
            out += self._unpack(raw[offs[i]:offs[i + 1]], want, name)
        return bytes(out)

    @staticmethod
    def _unpack(chunk, want, name):
        if len(chunk) == want:  # a sector stored raw inside a compressed file
            return chunk
        method = chunk[0]
        if method == 0x02:
            return zlib.decompress(chunk[1:])
        raise NotImplementedError(f'{name}: compression 0x{method:02x} is not supported (zlib only)')

    def listfile(self):
        if not self.has('(listfile)'):
            return []
        text = self.read('(listfile)').decode('latin-1')
        return [ln.strip() for ln in text.replace(';', '\n').splitlines() if ln.strip()]
