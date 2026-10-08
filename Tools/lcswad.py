"""Read the GTA: Liberty City Stories Android OBBs (War Drum's WAD container) and their textures.

    python Tools/lcswad.py list  <main.obb> [--patch <patch.obb>]
    python Tools/lcswad.py names <main.obb>            # how many HD textures have names

THE CONTAINER, as libGTALcs.so reads it (WadArchive, FSWadFile; v2.2):

- The whole file is XORed with the two bytes AF 66, by absolute file position
  (SerialiseEncryptDecryptBuffer: key[(pos + i) & 1]). Nothing else is encrypted.
- 0x000: "DAWL", u32 version 1, u32 1, zero to 0x800.
- 0x800: u32 node count, then a u32 pair per folder node. The names those pairs point at were not
  shipped.
- Then the FAT, 48 bytes an entry, sorted by hash: u32 hash, u32 name offset (unused), u32 folder
  node, u32 CRC, u64 offset, u64 size, u64 size again, u64 zero. The table ends at the first entry
  with a zero size.
- A file is found by hash only (WadArchive::GetFatEntry: bsearch): CRC32 with an initial ~0 and no
  final XOR, over the lower-case path with forward slashes. So names are recovered by hashing
  guesses: "models/gta3.img" is 0xcb58a66e.

THE HD TEXTURES (GetTXDTextureGetTexture): "Textures_ETC/<txd>/<texture>.pvr", where <txd>/<texture>
is the PSP texture's own name, or what master_texture_mapping_process.csv (in the OBB) remaps it
to. Each is a legacy PVR (52-byte header ending "PVR!"), ETC1 for opaque art and RGBA4444 for art
with alpha, with a full mip chain. This OBB has no DXT or PVRTC set.

Names come from the mapping CSV and from the PSP TXDs the OBB also carries (models/gta3.img and its
.dir); the world textures' folders are named in the level files, which are not read here, so those
keep their folder node and hash.
"""

import argparse
import collections
import struct
import zlib

import numpy as np

KEY = bytes([0xAF, 0x66])
_KEY = np.frombuffer(KEY * 4096, dtype=np.uint8)


def xor(data, position):
    """Undo the OBB's XOR for `data` read at file position `position`."""
    a = np.frombuffer(data, dtype=np.uint8)
    out = np.empty_like(a)
    s = position & 1
    step = len(_KEY) - 2
    for i in range(0, len(a), step):
        chunk = a[i:i + step]
        out[i:i + step] = chunk ^ _KEY[s:s + len(chunk)]
    return out.tobytes()


def path_hash(path):
    """The FAT key for a path: CRC32, initial ~0, no final XOR, lower case, forward slashes."""
    return zlib.crc32(path.replace('\\', '/').lower().encode('latin-1')) ^ 0xFFFFFFFF


class Wad:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        head = xor(self.f.read(0x800 + 4), 0)
        if head[:4] != b'DAWL':
            raise ValueError('%s is not a WAD (no DAWL after the XOR)' % path)
        n, = struct.unpack_from('<I', head, 0x800)
        self.f.seek(0)
        table = xor(self.f.read(0x808 + 8 * n + 48 * 65536), 0)
        self.nodes = [struct.unpack_from('<II', table, 0x808 + 8 * i) for i in range(n)]
        self.entries = []
        o = 0x808 + 8 * n
        while o + 48 <= len(table):
            hsh, nameoff, node, crc = struct.unpack_from('<IIII', table, o)
            offset, size = struct.unpack_from('<QQ', table, o + 16)
            if size == 0:
                break
            self.entries.append(dict(hash=hsh, node=node, crc=crc, offset=offset, size=size))
            o += 48
        self.by_hash = {e['hash']: e for e in self.entries}

    def find(self, path):
        return self.by_hash.get(path_hash(path))

    def read(self, entry, length=None, start=0):
        n = entry['size'] - start if length is None else min(length, entry['size'] - start)
        pos = entry['offset'] + start
        self.f.seek(pos)
        return xor(self.f.read(n), pos)


class Archives:
    """main + patch, the patch winning for a hash both carry, as the game mounts them."""

    def __init__(self, main, patch=None):
        self.wads = [Wad(main)] + ([Wad(patch)] if patch else [])
        self.by_hash = {}
        for w in self.wads:
            for e in w.entries:
                self.by_hash[e['hash']] = (w, e)

    def find(self, path):
        return self.by_hash.get(path_hash(path))

    def read(self, found, length=None, start=0):
        w, e = found
        return w.read(e, length, start)

    def items(self):
        return self.by_hash.items()


# --- Legacy PVR (v2) ---

PVR_ETC1 = 0x36
PVR_RGBA4444 = 0x10
_ETC_TABLES = np.array([[2, 8], [5, 17], [9, 29], [13, 42], [18, 60], [24, 80], [33, 106], [47, 183]], np.int32)


def pvr_header(data):
    (size, height, width, mips, flags, data_size, bpp, _r, _g, _b, amask, tag, _s) = struct.unpack_from('<13I', data, 0)
    if size != 52 or tag != 0x21525650:
        raise ValueError('not a legacy PVR')
    return dict(width=width, height=height, mips=mips, format=flags & 0xFF, alpha=amask != 0)


def decode_etc1(data, w, h):
    """ETC1 (big-endian 64-bit blocks in raster order) -> (h, w, 4) uint8."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    n = bw * bh
    raw = np.frombuffer(data[:n * 8].ljust(n * 8, b'\0'), dtype='>u8')
    hi = (raw >> np.uint64(32)).astype(np.uint32)
    lo = (raw & np.uint64(0xFFFFFFFF)).astype(np.uint32)
    diff = ((hi >> 1) & 1).astype(bool)
    flip = (hi & 1).astype(bool)
    cw1, cw2 = (hi >> 5) & 7, (hi >> 2) & 7

    def x4(v):
        return ((v << 4) | v).astype(np.int32)

    def x5(v):
        return ((v << 3) | (v >> 2)).astype(np.int32)

    def sdelta(v):
        v = v.astype(np.int32)
        return np.where(v >= 4, v - 8, v)

    c5 = [((hi >> s) & 0x1F).astype(np.int32) for s in (27, 19, 11)]
    dc = [sdelta((hi >> s) & 7) for s in (24, 16, 8)]
    base1 = np.stack([np.where(diff, x5(c5[i]), x4((hi >> (28 - 8 * i)) & 0xF)) for i in range(3)], -1)
    base2 = np.stack([np.where(diff, x5(np.clip(c5[i] + dc[i], 0, 31)), x4((hi >> (24 - 8 * i)) & 0xF))
                      for i in range(3)], -1)

    p = np.arange(16, dtype=np.uint32)                    # pixel p = x * 4 + y
    idx = (((lo[:, None] >> (p + 16)) & 1) << 1) | ((lo[:, None] >> p) & 1)
    x, y = (p // 4).astype(np.int32), (p % 4).astype(np.int32)
    second = np.where(flip[:, None], y[None, :] >= 2, x[None, :] >= 2)
    cw = np.where(second, cw2[:, None], cw1[:, None]).astype(np.int32)
    small, large = _ETC_TABLES[cw, 0], _ETC_TABLES[cw, 1]
    mod = np.select([idx == 0, idx == 1, idx == 2], [small, large, -small], -large)
    base = np.where(second[..., None], base2[:, None, :], base1[:, None, :])
    rgb = np.clip(base + mod[..., None], 0, 255).astype(np.uint8)

    out = np.full((bh * 4, bw * 4, 4), 255, np.uint8)
    out[..., :3] = rgb.reshape(bh, bw, 4, 4, 3).transpose(0, 3, 1, 2, 4).reshape(bh * 4, bw * 4, 3)
    return out[:h, :w]


def decode_rgba4444(data, w, h):
    v = np.frombuffer(data[:w * h * 2], dtype='<u2').reshape(h, w)
    return np.stack([((v >> s) & 0xF).astype(np.uint8) * 17 for s in (12, 8, 4, 0)], -1)


def decode_pvr(data):
    """The top level of a legacy PVR -> (h, w, 4) uint8 RGBA, and its header."""
    hd = pvr_header(data)
    body = data[52:]
    if hd['format'] == PVR_ETC1:
        return decode_etc1(body, hd['width'], hd['height']), hd
    if hd['format'] == PVR_RGBA4444:
        return decode_rgba4444(body, hd['width'], hd['height']), hd
    raise ValueError('PVR pixel format %#x is not handled' % hd['format'])


# --- Names ---

def _txd_texture_names(txd):
    """Names of the textures in a RenderWare TXD: the first string chunk of each texture native."""
    names = []
    o = 12
    while o + 12 <= len(txd):
        t, s, _v = struct.unpack_from('<III', txd, o)
        if t == 0x15:
            q, q_end = o + 12, o + 12 + s
            while q + 12 <= q_end:
                t2, s2, _v2 = struct.unpack_from('<III', txd, q)
                if t2 == 0x2:
                    names.append(txd[q + 12:q + 12 + s2].split(b'\0')[0].decode('latin-1'))
                    break
                q += 12 + s2
        if s > len(txd):
            break
        o += 12 + s
    return names


def hd_texture_names(arch):
    """{hash: 'txd/texture'} for every HD texture whose name can be recovered."""
    candidates = set()
    csv = arch.find('master_texture_mapping_process.csv')
    if csv:
        for line in arch.read(csv).decode('latin-1').splitlines():
            key = line.split(',')[0].strip()
            if '/' in key:
                candidates.add(key)
    img, dirf = arch.find('models/gta3.img'), arch.find('models/gta3.dir')
    if img and dirf:
        d = arch.read(dirf)
        for i in range(len(d) // 32):
            off, size = struct.unpack_from('<II', d, i * 32)
            name = d[i * 32 + 8:i * 32 + 32].split(b'\0')[0].decode('latin-1')
            if name.lower().endswith('.txd'):
                txd = arch.read(img, size * 2048, off * 2048)
                for tex in _txd_texture_names(txd):
                    candidates.add(name[:-4] + '/' + tex)
    out = {}
    for c in candidates:
        h = path_hash('textures_etc/%s.pvr' % c)
        if h in arch.by_hash:
            out[h] = c
    return out


def hd_textures(arch):
    """Every HD texture: [(hash, (wad, entry))], told apart from other files by the PVR header."""
    found = []
    for h, (w, e) in arch.items():
        if e['size'] > 52 and w.read(e, 4) == b'\x34\x00\x00\x00':
            found.append((h, (w, e)))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['list', 'names'])
    ap.add_argument('obb')
    ap.add_argument('--patch')
    args = ap.parse_args()
    arch = Archives(args.obb, args.patch)
    if args.command == 'list':
        kinds = collections.Counter()
        for _h, (w, e) in arch.items():
            kinds[w.read(e, 4)[:4]] += 1
        print('%d files' % len(arch.by_hash))
        for k, v in kinds.most_common(12):
            print('  %-14r %d' % (k, v))
    else:
        tex = hd_textures(arch)
        names = hd_texture_names(arch)
        print('%d HD textures, %d named' % (len(tex), sum(1 for h, _ in tex if h in names)))


if __name__ == '__main__':
    main()
