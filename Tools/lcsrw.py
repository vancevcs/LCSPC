"""Read RenderWare texture dictionaries (.txd) and decode their rasters to RGBA.

Ported from the VCS fork's tools/txd.py (gtavcspc 1193f89) for the LCS texture tool
(Tools/lcstxd.py). One addition: the Android OBB is a source too. `scan_tree` on the
main .obb lists its HD PVRs (Tools/lcswad.py) as the textures of dictionaries named
android/<txd>, and `TextureInfo.load` decodes them a level at a time.

Covers the PC (Direct3D 8 and 9) rasters that GTA III / Vice City / San Andreas
and their texture mods ship. The Vice City HD packs are uniformly D3D8 + DXT,
but the uncompressed and palettised paths are here too, because stock VC TXDs
use 8888/888 and PAL8, and a mod folder usually turns out to be a mixture.

Not covered: PS2, Xbox and PSP native rasters. Those store swizzled pixel data
behind a completely different struct, and nothing in the Vice City PC line uses
them -- a TXD carrying one raises UnsupportedRaster rather than decoding noise.

DXT blocks decode on the CPU with numpy rather than through Pillow's DDS
reader. The reason is DXT1: the RenderWare raster header says whether a texture
uses the punch-through alpha variant, and honouring that flag is what keeps
opaque DXT1 textures from picking up stray transparent texels wherever a block
happens to be in three-colour mode.

Loose image files (a PCSX2 replacement dump, an upscaler's output folder) are
also presented as textures, so the rest of the pipeline can treat a folder of
PNGs and a folder of dictionaries the same way -- see `texture_from_image`.

Usage as a library:

    from txd import read_txd
    for tex in read_txd("airport.txd"):
        tex.image().save(tex.name + ".png")   # PIL RGBA Image, mip level 0
"""

import glob
import os
import struct
import sys
import threading

import numpy as np

# ---------------------------------------------------------------- chunk types

CHUNK_STRUCT = 0x01
CHUNK_EXTENSION = 0x03
CHUNK_TEXTURE_NATIVE = 0x15
CHUNK_TEXTURE_DICTIONARY = 0x16

PLATFORM_D3D8 = 8
PLATFORM_D3D9 = 9

RASTER_TEXTURE = 4          # rwRASTERTYPETEXTURE

# ------------------------------------------------------------- raster formats

FORMAT_MASK = 0x0F00
FMT_DEFAULT = 0x0000
FMT_1555 = 0x0100
FMT_565 = 0x0200
FMT_4444 = 0x0300
FMT_LUM8 = 0x0400
FMT_8888 = 0x0500
FMT_888 = 0x0600
FMT_16 = 0x0700
FMT_24 = 0x0800
FMT_32 = 0x0900
FMT_555 = 0x0A00

EXT_AUTO_MIPMAP = 0x1000
EXT_PAL8 = 0x2000
EXT_PAL4 = 0x4000
EXT_MIPMAP = 0x8000

FORMAT_NAMES = {
    FMT_DEFAULT: "default", FMT_1555: "1555", FMT_565: "565", FMT_4444: "4444",
    FMT_LUM8: "lum8", FMT_8888: "8888", FMT_888: "888", FMT_16: "16",
    FMT_24: "24", FMT_32: "32", FMT_555: "555",
}

FOURCC_DXT = {0x31545844: 1, 0x33545844: 3, 0x35545844: 5}

# Loose image files usable as texture sources. A TextureInfo with this offset
# means "the whole file is the raster" rather than "a chunk at this address".
IMAGE_EXTS = (".png", ".bmp", ".tga", ".jpg", ".jpeg", ".dds")
LOOSE = -1

# Folders that hold images which are not game art. A shader pack's noise and
# lookup tables would otherwise be indexed as several dozen candidate textures.
SKIP_DIRS = {"reshade-shaders", "screenshots", "logs", "__pycache__", ".git"}
MIN_IMAGE_SIDE = 8          # below this it is an icon or a spacer, not art


class TxdError(Exception):
    """The file is not a texture dictionary, or is truncated."""


class UnsupportedRaster(TxdError):
    """A raster in a platform format this reader does not decode."""


def unpack_version(lib):
    """RenderWare packs its version into the high bits of the library id."""
    if lib & 0xFFFF0000:
        return (lib >> 14 & 0x3FF00) | (lib >> 16 & 0x3F) | 0x30000
    return lib << 8


def version_string(lib):
    v = unpack_version(lib)
    return f"{v >> 16 & 0xF}.{v >> 12 & 0xF}.{v >> 8 & 0xF}.{v & 0x3F}"


# --------------------------------------------------------------- DXT decoding

def _expand565(c):
    """5:6:5 -> three uint16 channels, high bits replicated into the low end."""
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def _blocks_to_image(blocks, bw, bh, w, h, channels):
    """(bh, bw, 16, C) block-major texels -> (h, w, C) image, cropped to size."""
    img = blocks.reshape(bh, bw, 4, 4, channels)
    img = img.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, channels)
    return np.ascontiguousarray(img[:h, :w])


def decode_dxt(data, w, h, kind, punchthrough):
    """Decode a BC1/BC2/BC3 surface to an (h, w, 4) uint8 RGBA array.

    `kind` is the RenderWare dxtCompression byte: 1, 3 or 5. `punchthrough`
    only matters for DXT1 -- when false, three-colour blocks still yield an
    opaque texel instead of a transparent one.
    """
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    stride = 8 if kind == 1 else 16
    need = bw * bh * stride
    if len(data) < need:
        data = bytes(data) + b"\0" * (need - len(data))
    buf = np.frombuffer(data[:need], np.uint8).reshape(bh, bw, stride)

    colour = buf[:, :, stride - 8:]
    c0 = colour[:, :, 0].astype(np.uint16) | (colour[:, :, 1].astype(np.uint16) << 8)
    c1 = colour[:, :, 2].astype(np.uint16) | (colour[:, :, 3].astype(np.uint16) << 8)
    bits = (colour[:, :, 4].astype(np.uint32)
            | (colour[:, :, 5].astype(np.uint32) << 8)
            | (colour[:, :, 6].astype(np.uint32) << 16)
            | (colour[:, :, 7].astype(np.uint32) << 24))

    e0, e1 = _expand565(c0), _expand565(c1)
    # DXT2/3/5 always use four-colour interpolation; only DXT1 switches on the
    # endpoint ordering, and that same ordering is what selects its 1-bit alpha.
    four = (c0 > c1) if kind == 1 else np.ones(c0.shape, bool)
    pal = np.empty((bh, bw, 4, 3), np.uint16)
    pal[:, :, 0] = e0
    pal[:, :, 1] = e1
    pal[:, :, 2] = np.where(four[..., None], (2 * e0 + e1) // 3, (e0 + e1) // 2)
    pal[:, :, 3] = np.where(four[..., None], (e0 + 2 * e1) // 3, 0)

    idx = ((bits[..., None] >> (2 * np.arange(16, dtype=np.uint32))) & 3).astype(np.intp)
    rgb = np.take_along_axis(pal, idx[..., None], axis=2).astype(np.uint8)

    if kind == 1:
        clear = (~four)[..., None] & (idx == 3) if punchthrough else np.zeros(idx.shape, bool)
        alpha = np.where(clear, 0, 255).astype(np.uint8)[..., None]
    elif kind == 3:
        nib = buf[:, :, :8, None] >> np.array([0, 4], np.uint8)
        alpha = ((nib.reshape(bh, bw, 16) & 0xF) * 17).astype(np.uint8)[..., None]
    else:
        a0 = buf[:, :, 0].astype(np.uint16)
        a1 = buf[:, :, 1].astype(np.uint16)
        wide = a0 > a1
        ramp = np.empty((bh, bw, 8), np.uint16)
        ramp[:, :, 0] = a0
        ramp[:, :, 1] = a1
        for i in range(1, 7):
            eight = ((7 - i) * a0 + i * a1) // 7
            six = ((5 - i) * a0 + i * a1) // 5
            ramp[:, :, i + 1] = np.where(wide, eight, six)
        # In the narrow mode the last two slots are the constants 0 and 255,
        # not interpolated endpoints.
        ramp[:, :, 6] = np.where(wide, ramp[:, :, 6], 0)
        ramp[:, :, 7] = np.where(wide, ramp[:, :, 7], 255)

        abits = np.zeros((bh, bw), np.uint64)
        for i in range(6):
            abits |= buf[:, :, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        aidx = ((abits[..., None] >> (3 * np.arange(16, dtype=np.uint64))) & 7).astype(np.intp)
        alpha = np.take_along_axis(ramp, aidx, axis=2).astype(np.uint8)[..., None]

    return _blocks_to_image(np.concatenate([rgb, alpha], -1), bw, bh, w, h, 4)


# -------------------------------------------------- uncompressed / palettised

def _decode_palette(data, palette, w, h, depth):
    if depth == 4:
        packed = np.frombuffer(data[:(w * h + 1) // 2], np.uint8)
        idx = np.empty(packed.size * 2, np.uint8)
        idx[0::2] = packed & 0xF
        idx[1::2] = packed >> 4
    else:
        idx = np.frombuffer(data[:w * h], np.uint8)
    if idx.size < w * h:
        idx = np.pad(idx, (0, w * h - idx.size))
    return palette[idx[:w * h]].reshape(h, w, 4)


def _decode_raw(data, w, h, fmt, depth):
    """Decode a linear D3D surface. RenderWare stores these BGRA-ordered."""
    n = w * h
    if fmt in (FMT_8888, FMT_32) or (fmt == FMT_DEFAULT and depth == 32):
        px = np.frombuffer(data[:n * 4], np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]]
    elif fmt in (FMT_888, FMT_24) or (fmt == FMT_DEFAULT and depth == 24):
        # 888 is commonly still stored 32 bits wide with a padding byte.
        if depth == 32:
            src = np.frombuffer(data[:n * 4], np.uint8).reshape(-1, 4)[:, [2, 1, 0]]
        else:
            src = np.frombuffer(data[:n * 3], np.uint8).reshape(-1, 3)[:, ::-1]
        px = np.concatenate([src, np.full((src.shape[0], 1), 255, np.uint8)], 1)
    elif fmt == FMT_LUM8:
        lum = np.frombuffer(data[:n], np.uint8)
        px = np.stack([lum, lum, lum, np.full(lum.size, 255, np.uint8)], -1)
    elif fmt in (FMT_1555, FMT_565, FMT_4444, FMT_555, FMT_16):
        v = np.frombuffer(data[:n * 2], "<u2").astype(np.uint32)
        if fmt == FMT_565:
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            r, g, b = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)
            a = np.full(v.size, 255, np.uint32)
        elif fmt == FMT_4444:
            a, r = (v >> 12) & 0xF, (v >> 8) & 0xF
            g, b = (v >> 4) & 0xF, v & 0xF
            r, g, b, a = r * 17, g * 17, b * 17, a * 17
        else:  # 1555 and 555 share the channel layout, differ on the top bit
            r, g, b = (v >> 10) & 0x1F, (v >> 5) & 0x1F, v & 0x1F
            r, g, b = (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)
            a = (np.where((v >> 15) & 1, 255, 0) if fmt == FMT_1555
                 else np.full(v.size, 255, np.uint32))
        px = np.stack([r, g, b, a], -1).astype(np.uint8)
    else:
        raise UnsupportedRaster(f"raster format {fmt:#06x} at depth {depth}")

    if px.shape[0] < n:
        px = np.pad(px, ((0, n - px.shape[0]), (0, 0)))
    return px[:n].reshape(h, w, 4)


# ------------------------------------------------------------------- textures

class Texture:
    """One raster out of a texture dictionary, decoded on demand."""

    def __init__(self, name, mask, platform, raster_format, width, height,
                 depth, levels, raster_type, compression, has_alpha,
                 palette, mips, source=""):
        self.name = name
        self.mask = mask
        self.platform = platform
        self.raster_format = raster_format
        self.width = width
        self.height = height
        self.depth = depth
        self.levels = levels
        self.raster_type = raster_type
        self.compression = compression
        self.has_alpha = has_alpha
        self.palette = palette
        self.mips = mips            # [(w, h, bytes)], largest first
        self.source = source        # the .txd this came from

    @property
    def format_name(self):
        base = FORMAT_NAMES.get(self.raster_format & FORMAT_MASK, "?")
        if self.compression:
            return f"DXT{self.compression}"
        if self.raster_format & EXT_PAL8:
            return f"PAL8/{base}"
        if self.raster_format & EXT_PAL4:
            return f"PAL4/{base}"
        return base

    @property
    def data_size(self):
        return sum(len(m[2]) for m in self.mips)

    def array(self, level=0):
        """Decode one mip level to an (h, w, 4) uint8 RGBA array."""
        w, h, data = self.mips[level]
        if self.compression:
            return decode_dxt(data, w, h, self.compression, bool(self.has_alpha))
        if self.raster_format & (EXT_PAL4 | EXT_PAL8):
            return _decode_palette(data, self.palette, w, h,
                                   4 if self.raster_format & EXT_PAL4 else 8)
        return _decode_raw(data, w, h, self.raster_format & FORMAT_MASK, self.depth)

    def image(self, level=0):
        """Decode one mip level to a PIL RGBA Image."""
        from PIL import Image
        return Image.fromarray(self.array(level), "RGBA")

    def level_for(self, max_side):
        """Smallest mip level still at least `max_side` across.

        Decoding a 2048x2048 base level to draw a 128px thumbnail is most of
        what a texture browser would otherwise spend its time on. The mip chain
        is already in the file, so use it.
        """
        best = 0
        for i, (w, h, _) in enumerate(self.mips):
            if max(w, h) >= max_side:
                best = i
            else:
                break
        return best

    def alpha_range(self):
        """(min, max) alpha over the base level -- (255, 255) means opaque."""
        a = self.array()[..., 3]
        return int(a.min()), int(a.max())

    def __repr__(self):
        return (f"<Texture {self.name!r} {self.width}x{self.height} "
                f"{self.format_name} levels={self.levels}>")


def texture_from_image(img, name, source=""):
    """Wrap a PIL image as a Texture, so a loose file reads like a TXD raster.

    Rather than introduce a parallel "loose texture" type for the browser and
    the matcher to special-case, the pixels are handed to the ordinary Texture
    as a single-level 8888 raster. Everything downstream -- mip lists, alpha
    ranges, format labels, `level_for` -- then works unchanged.

    RenderWare stores 8888 as BGRA, so the channels are swapped on the way in
    to match what `_decode_raw` will do on the way out.
    """
    img = img.convert("RGBA")
    w, h = img.size
    pixels = np.asarray(img, np.uint8)[..., [2, 1, 0, 3]]
    lo = int(pixels[..., 3].min()) if pixels.size else 255
    return Texture(name, "", PLATFORM_D3D9, FMT_8888, w, h, 32, 1,
                   RASTER_TEXTURE, 0, lo < 255, None,
                   [(w, h, pixels.tobytes())], source)


# --------------------------------------------------------------------- reader

class TextureList(list):
    """The textures of one dictionary, plus what was skipped getting them."""

    def __init__(self, items=(), version="", skipped=()):
        super().__init__(items)
        self.version = version
        self.skipped = list(skipped)

    def by_name(self, name):
        low = name.lower()
        return next((t for t in self if t.name.lower() == low), None)


def _chunks(data, start, end):
    """Yield (type, payload_start, payload_end, library_id) for one level."""
    off = start
    while off + 12 <= end:
        ctype, size, lib = struct.unpack_from("<III", data, off)
        stop = off + 12 + size
        if stop > end:
            raise TxdError(f"chunk {ctype:#x} at {off:#x} overruns its parent")
        yield ctype, off + 12, stop, lib
        off = stop


def _read_native(data, start, end, source):
    for ctype, body, _stop, _lib in _chunks(data, start, end):
        if ctype != CHUNK_STRUCT:
            continue
        platform, _filter = struct.unpack_from("<II", data, body)
        if platform not in (PLATFORM_D3D8, PLATFORM_D3D9):
            raise UnsupportedRaster(
                f"{os.path.basename(source)}: platform {platform:#x} raster "
                "(PS2/Xbox/PSP native) is not decodable here")

        name = data[body + 8:body + 40].split(b"\0")[0].decode("latin-1")
        mask = data[body + 40:body + 72].split(b"\0")[0].decode("latin-1")
        raster_format, d3d, width, height, depth, levels, rtype, flags = \
            struct.unpack_from("<IIHHBBBB", data, body + 72)
        off = body + 88

        if platform == PLATFORM_D3D8:
            # In D3D8 the trailing byte *is* the DXT number, and the uint32
            # before the dimensions is a plain alpha flag, not a D3DFORMAT.
            compression, has_alpha = flags, bool(d3d)
        else:
            compression = FOURCC_DXT.get(d3d, 0)
            has_alpha = bool(flags & 0x01)
            if flags & 0x02:
                raise UnsupportedRaster(f"{name}: cube maps are not supported")

        palette = None
        if raster_format & (EXT_PAL4 | EXT_PAL8):
            # RenderWare always allocates 32 entries for a 4-bit palette.
            count = 32 if raster_format & EXT_PAL4 else 256
            raw = np.frombuffer(data[off:off + count * 4], np.uint8).reshape(-1, 4)
            palette = raw[:, [2, 1, 0, 3]].copy()
            off += count * 4

        mips, w, h = [], width, height
        for _ in range(max(levels, 1)):
            (size,) = struct.unpack_from("<I", data, off)
            off += 4
            mips.append((max(w, 1), max(h, 1), data[off:off + size]))
            off += size
            w, h = max(w // 2, 1), max(h // 2, 1)

        return Texture(name, mask, platform, raster_format, width, height, depth,
                       max(levels, 1), rtype, compression, has_alpha, palette,
                       mips, source)
    raise TxdError(f"{os.path.basename(source)}: texture native has no struct chunk")


def read_txd_bytes(data, source="<memory>", skip_unsupported=True):
    """Parse texture dictionary bytes into a TextureList."""
    if len(data) < 12:
        raise TxdError(f"{source}: too small to be a TXD")
    ctype, size, lib = struct.unpack_from("<III", data, 0)
    if ctype != CHUNK_TEXTURE_DICTIONARY:
        raise TxdError(f"{source}: not a texture dictionary "
                       f"(first chunk is {ctype:#x}, expected 0x16)")
    end = min(12 + size, len(data))

    textures, skipped = [], []
    for kind, body, stop, _lib in _chunks(data, 12, end):
        if kind != CHUNK_TEXTURE_NATIVE:
            continue
        try:
            textures.append(_read_native(data, body, stop, source))
        except UnsupportedRaster as exc:
            if not skip_unsupported:
                raise
            skipped.append(str(exc))
    return TextureList(textures, version_string(lib), skipped)


def read_txd(path, skip_unsupported=True):
    """Parse a .txd file and return its TextureList.

    A raster this reader cannot decode is skipped and recorded in
    `result.skipped` rather than failing the whole dictionary -- one odd
    texture should not cost you the other two hundred in the file.
    """
    with open(path, "rb") as fh:
        return read_txd_bytes(fh.read(), path, skip_unsupported)


# ----------------------------------------------------- indexing without decode

class TextureInfo:
    """What a texture is, without having read a byte of its pixels.

    Scanning 760 dictionaries this way costs a few seconds and a few megabytes,
    where reading them whole costs 2.6 GB. `load()` goes back for the one
    raster you actually want to look at.
    """

    __slots__ = ("name", "mask", "width", "height", "format_name", "levels",
                 "has_alpha", "source", "offset", "chunk_size", "container")

    def __init__(self, name, mask, width, height, format_name, levels,
                 has_alpha, source, offset, chunk_size, container=""):
        self.name = name
        self.mask = mask
        self.width = width
        self.height = height
        self.format_name = format_name
        self.levels = levels
        self.has_alpha = has_alpha
        self.source = source
        self.offset = offset            # absolute file offset of the chunk
        self.chunk_size = chunk_size
        self.container = container      # the .txd's name when inside an archive

    @property
    def is_loose(self):
        """True when this is a standalone image file rather than a TXD raster."""
        return self.offset == LOOSE

    @property
    def label(self):
        """What to call the dictionary this came from, archive or not.

        A loose image has no dictionary, so it is grouped by the folder it was
        found in -- otherwise a 2,000-file replacement dump would arrive as two
        thousand one-texture "dictionaries" in the browser's left pane.
        """
        if self.is_loose:
            return self.container or os.path.basename(os.path.dirname(self.source))
        if is_obb(self.source):
            return f"android/{self.container}"
        if self.container:
            return f"{os.path.basename(self.source)}/{self.container}"
        return os.path.basename(self.source)

    def load(self):
        """Read and parse just this texture, returning a full Texture.

        The offset is absolute, so a texture inside an IMG archive loads by
        exactly the same path as one in a loose .txd -- the archive is only a
        base address, and nothing below this needs to know about it.
        """
        if self.is_loose:
            from PIL import Image
            with Image.open(self.source) as img:
                return texture_from_image(img, self.name, self.label)
        if is_obb(self.source):
            return _load_obb(self)
        with open(self.source, "rb") as fh:
            fh.seek(self.offset)
            head = fh.read(12)
            _ctype, size, _lib = struct.unpack_from("<III", head, 0)
            body = fh.read(size)
        data = head + body
        return _read_native(data, 12, len(data), self.label)

    def __repr__(self):
        return (f"<TextureInfo {self.name!r} {self.width}x{self.height} "
                f"{self.format_name} in {self.label}>")


def _scan_stream(fh, base, source, container="", limit=None):
    """Texture headers of one dictionary starting at `base` in an open file."""
    infos = []
    fh.seek(base)
    head = fh.read(12)
    if len(head) < 12:
        raise TxdError(f"{source}: too small to be a TXD")
    ctype, size, _lib = struct.unpack_from("<III", head, 0)
    if ctype != CHUNK_TEXTURE_DICTIONARY:
        raise TxdError(f"{container or source}: not a texture dictionary "
                       f"(first chunk is {ctype:#x}, expected 0x16)")
    end = base + 12 + size
    if limit is not None:
        end = min(end, base + limit)

    off = base + 12
    while off + 12 <= end:
        fh.seek(off)
        hdr = fh.read(12)
        if len(hdr) < 12:
            break
        kind, csize, _ = struct.unpack_from("<III", hdr, 0)
        if kind == CHUNK_TEXTURE_NATIVE:
            # struct chunk header, then the 88-byte raster header
            raster = fh.read(12 + 88)
            if len(raster) >= 100:
                platform = struct.unpack_from("<I", raster, 12)[0]
                name = raster[20:52].split(b"\0")[0].decode("latin-1")
                mask = raster[52:84].split(b"\0")[0].decode("latin-1")
                rfmt, d3d, w, h, _depth, levels, _rt, flags = \
                    struct.unpack_from("<IIHHBBBB", raster, 84)
                if platform == PLATFORM_D3D8:
                    comp, alpha = flags, bool(d3d)
                elif platform == PLATFORM_D3D9:
                    comp, alpha = FOURCC_DXT.get(d3d, 0), bool(flags & 0x01)
                else:
                    comp, alpha = 0, False
                if platform in (PLATFORM_D3D8, PLATFORM_D3D9):
                    fmt = FORMAT_NAMES.get(rfmt & FORMAT_MASK, "?")
                    if comp:
                        fmt = f"DXT{comp}"
                    elif rfmt & EXT_PAL8:
                        fmt = f"PAL8/{fmt}"
                    elif rfmt & EXT_PAL4:
                        fmt = f"PAL4/{fmt}"
                    infos.append(TextureInfo(name, mask, w, h, fmt,
                                             max(levels, 1), alpha, source,
                                             off, csize, container))
        off += 12 + csize
    return infos


def scan_txd(path):
    """List a loose dictionary's textures by seeking, without decoding pixels."""
    with open(path, "rb") as fh:
        return _scan_stream(fh, 0, path)


# ------------------------------------------------------------- IMG archives

IMG_MAGIC = b"VER2"
IMG_SECTOR = 2048
IMG_ENTRY = 32


def read_img_directory(path):
    """Entries of an IMG archive, as (name, offset, size). Both versions.

    Version 2 is what San Andreas ships and what the upscale packs rebuild: a
    "VER2" magic, an entry count, then fixed 32-byte records of offset and size
    in 2048-byte sectors followed by a 24-byte name.

    Version 1 is the GTA III / Vice City layout, and the archive itself carries
    no header at all -- it opens directly on its first chunk. The same 32-byte
    records live in a sibling .dir instead, where the record's second field is a
    single size rather than the streaming/archived pair. HD texture packs for
    those games still ship this way, so an archive without the magic is tried as
    a version 1 before it is called malformed.
    """
    with open(path, "rb") as fh:
        head = fh.read(8)
        if head[:4] == IMG_MAGIC:
            count = struct.unpack_from("<I", head, 4)[0]
            table, version = fh.read(count * IMG_ENTRY), 2
        else:
            dir_path = os.path.splitext(path)[0] + ".dir"
            if not os.path.exists(dir_path):
                raise TxdError(f"{path}: not a version-2 IMG (magic {head[:4]!r}) "
                               f"and no {os.path.basename(dir_path)} beside it")
            with open(dir_path, "rb") as dh:
                table = dh.read()
            count, version = len(table) // IMG_ENTRY, 1

    entries = []
    for i in range(count):
        base = i * IMG_ENTRY
        if base + IMG_ENTRY > len(table):
            break
        if version == 2:
            offset, streaming, archived = struct.unpack_from("<IHH", table, base)
            # Rebuilt archives commonly leave one of the two size fields at zero.
            sectors = archived or streaming
        else:
            offset, sectors = struct.unpack_from("<II", table, base)
        name = table[base + 8:base + IMG_ENTRY].split(b"\0")[0].decode("latin-1")
        entries.append((name, offset * IMG_SECTOR, sectors * IMG_SECTOR))
    return entries


def scan_img(path, on_error=None):
    """Texture headers for every dictionary inside an IMG archive.

    The archive is opened once and every dictionary is scanned through the same
    handle. Offsets recorded in the results are absolute, so the textures load
    afterwards without the archive being involved again.
    """
    infos = []
    with open(path, "rb") as fh:
        for name, offset, size in read_img_directory(path):
            if not name.lower().endswith(".txd"):
                continue
            try:
                infos += _scan_stream(fh, offset, path, name, limit=size)
            except (TxdError, struct.error) as exc:
                if on_error:
                    on_error(f"{os.path.basename(path)}/{name}: {exc}")
    return infos


def scan_images(folder, root=None, on_error=None):
    """Loose image files under a folder, as TextureInfo with a LOOSE offset.

    An upscale project is not always shipped as dictionaries. A PCSX2 texture
    dump is a flat folder of PNGs, and a neural upscaler's output is the same,
    so a source tree holding thousands of usable textures can contain no TXD at
    all. Presenting those files as textures is what lets them be searched,
    previewed and matched alongside everything else.

    The name is the file's stem and the group is its folder relative to `root`,
    so entries stay distinct when two packs both contain a "textures" folder.
    """
    from PIL import Image
    root = os.path.abspath(root or folder)
    infos = []
    for dirpath, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d.lower() not in SKIP_DIRS)
        group = os.path.relpath(dirpath, os.path.dirname(root)).replace("\\", "/")
        for name in sorted(files):
            if not name.lower().endswith(IMAGE_EXTS):
                continue
            path = os.path.join(dirpath, name)
            try:
                with Image.open(path) as img:
                    width, height = img.size
                    fmt = (img.format or "img").lower()
            except Exception as exc:
                if on_error:
                    on_error(f"{name}: {exc}")
                continue
            if min(width, height) < MIN_IMAGE_SIDE:
                continue
            infos.append(TextureInfo(os.path.splitext(name)[0], "", width, height,
                                     fmt, 1, True, path, LOOSE, 0, group))
    return infos


def scan_any(path, on_error=None):
    """Scan a .txd, a .img or an Android .obb, whichever it is."""
    if path.lower().endswith(".img"):
        return scan_img(path, on_error)
    if is_obb(path):
        return scan_obb(path, on_error)
    return scan_txd(path)


def scan_tree(folder, on_error=None, progress=None, images=True):
    """Every texture under a folder: loose .txd files, IMG archives, and images.

    Directories are checked as well as extensions. A San Andreas mod install can
    legitimately contain a *folder* named "player.img", and walking into one as
    if it were an archive is an error worth not reporting.
    """
    if is_obb(folder) and os.path.isfile(folder):
        infos = scan_obb(folder, on_error)
        if progress:
            progress(1, 1)
        return infos
    targets = []
    for dirpath, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d.lower() not in SKIP_DIRS)
        for name in sorted(files):
            if name.lower().endswith((".txd", ".img")):
                path = os.path.join(dirpath, name)
                if os.path.isfile(path):
                    targets.append(path)
    targets.sort()

    steps = len(targets) + bool(images)
    infos = []
    for i, path in enumerate(targets):
        try:
            infos += scan_any(path, on_error)
        except (TxdError, OSError, struct.error) as exc:
            if on_error:
                on_error(f"{os.path.basename(path)}: {exc}")
        if progress:
            progress(i + 1, steps)
    if images:
        infos += scan_images(folder, folder, on_error)
        if progress:
            progress(steps, steps)
    return infos


# ------------------------------------------------------- the Android OBB

# LCS's Android build (v2.2) keeps its HD art as one legacy PVR per texture at
# Textures_ETC/<txd>/<texture>.pvr, inside an XOR'd "DAWL" WAD that lcswad.py reads.
# Listed here like the rasters of a TXD: the WAD entry's hash stands in for the chunk
# offset and the txd folder is the container, so a texture reloads from its
# TextureInfo alone. The patch OBB beside the main one is mounted over it, as the
# game does. Names come from lcswad.hd_texture_names; the world textures it cannot
# name keep their folder node and hash (folder1102/006857ce).

OBB_EXT = ".obb"
_obb_lock = threading.Lock()
_obb_archives = {}              # main .obb path -> (lcswad.Archives, read lock)
_obb_scans = {}                 # main .obb path -> [TextureInfo]


def is_obb(path):
    return str(path).lower().endswith(OBB_EXT)


def _lcswad():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import lcswad
    return lcswad


def _archives(path):
    """The OBB with its patch mounted, opened once per path and shared.

    A WAD is read through one file handle (seek, then read), so reads from the
    thumbnail pool take the lock that comes with it.
    """
    key = os.path.normcase(os.path.abspath(path))
    with _obb_lock:
        if key not in _obb_archives:
            lcswad = _lcswad()
            patches = sorted(glob.glob(os.path.join(os.path.dirname(path), "patch.*.obb")))
            _obb_archives[key] = (lcswad.Archives(path, patches[-1] if patches else None),
                                  threading.Lock())
        return _obb_archives[key]


class PvrTexture(Texture):
    """A PVR's mip chain, each level decoded only when it is asked for.

    The levels are kept as the PVR stores them (ETC1 blocks or RGBA4444), so a
    thumbnail decodes a 128px level and never the 512px top.
    """

    def __init__(self, name, header, levels, source=""):
        super().__init__(name, "", 0, FMT_8888, header["width"], header["height"], 32,
                         len(levels), RASTER_TEXTURE, 0, header["alpha"], None, levels,
                         source)
        self.header = header

    @property
    def format_name(self):
        return {0x36: "ETC1", 0x10: "RGBA4444"}.get(self.header["format"], "PVR")

    def array(self, level=0):
        w, h, data = self.mips[level]
        return _lcswad().decode_level(self.header, data, w, h)


def _load_obb(info):
    lcswad = _lcswad()
    arch, lock = _archives(info.source)
    wad, entry = arch.by_hash[int(info.offset)]
    with lock:
        data = wad.read(entry)
    header = lcswad.pvr_header(data)
    levels = []
    for level in range(header["mips"] + 1):
        offset, size, w, h = lcswad.pvr_level(header, level)
        levels.append((w, h, data[offset:offset + size]))
    return PvrTexture(info.name, header, levels, info.label)


def scan_obb(path, on_error=None):
    """Every HD texture in an Android OBB, as TextureInfo, without decoding one.

    Recovering the names reads every TXD in the OBB's gta3.img, which is most of
    the time this takes, so a scan is kept for the life of the process.
    """
    key = os.path.normcase(os.path.abspath(path))
    if key in _obb_scans:
        return list(_obb_scans[key])
    lcswad = _lcswad()
    arch, lock = _archives(path)
    infos = []
    with lock:
        names = lcswad.hd_texture_names(arch)
        for h, (wad, entry) in lcswad.hd_textures(arch):
            try:
                header = lcswad.pvr_header(wad.read(entry, 52))
            except (ValueError, struct.error) as exc:
                if on_error:
                    on_error(f"{os.path.basename(path)}/{h:08x}: {exc}")
                continue
            name = names.get(h, "folder%04d/%08x" % (entry["node"], h))
            folder, _, texture = name.rpartition("/")
            fmt = {0x36: "ETC1", 0x10: "RGBA4444"}.get(header["format"], "PVR")
            infos.append(TextureInfo(texture, "", header["width"], header["height"], fmt,
                                     header["mips"] + 1, header["alpha"], path, h, 0,
                                     folder))
    infos.sort(key=lambda i: (i.container.startswith("folder"), i.container, i.name))
    _obb_scans[key] = infos
    return list(infos)


# ------------------------------------------------------------- scan cache

# Scanning the VCS fork's sources folder (some 137,000 textures in loose TXDs, IMG
# archives and image files) took three minutes on a cold disk. The result is kept
# beside the source, named like lcsmatch's signature index, and keyed the same way:
# on the source's path, not its contents. Rescan after adding to a source (the
# browser's Reindex does).

_SCAN_FIELDS = ("name", "mask", "width", "height", "format_name", "levels",
                "has_alpha", "source", "offset", "chunk_size", "container")


def scan_cache_path(folder):
    import hashlib
    folder = os.path.abspath(folder)
    stamp = hashlib.blake2b(folder.encode(), digest_size=8).hexdigest()
    where = folder if os.path.isdir(folder) else os.path.dirname(folder)
    return os.path.join(where, f".txd_scan_{stamp}.npz")


def scan_tree_cached(folder, on_error=None, progress=None, rebuild=False):
    """scan_tree, read back from the last scan of the same source when there is one."""
    path = scan_cache_path(folder)
    if not rebuild and os.path.exists(path):
        try:
            z = np.load(path, allow_pickle=False)
            cols = [z[f].tolist() for f in _SCAN_FIELDS]
            return [TextureInfo(*row) for row in zip(*cols)]
        except Exception:
            pass
    infos = scan_tree(folder, on_error, progress)
    try:
        np.savez_compressed(path, **{f: np.array([getattr(i, f) for i in infos])
                                     for f in _SCAN_FIELDS})
    except (OSError, ValueError):
        pass
    return infos


# --------------------------------------------------------------------- writer

RW_VERSION_VC = 0x1003FFFF      # the library id Vice City's own TXDs carry


def _chunk(ctype, payload, lib=RW_VERSION_VC):
    return struct.pack("<III", ctype, len(payload), lib) + payload


def texture_native(name, image, mask="", lib=RW_VERSION_VC):
    """One D3D8 texture-native chunk holding an uncompressed 8888 raster.

    Written uncompressed on purpose. The point of these files is to be read
    back as source art -- by this module, by the browser, by anything that
    speaks RenderWare -- and re-encoding to DXT to save space on disk would
    throw away detail that the original raster still has.

    `image` is an (h, w, 4) uint8 RGBA array; RenderWare wants it BGRA.
    """
    pixels = np.ascontiguousarray(np.asarray(image, np.uint8)[..., [2, 1, 0, 3]])
    height, width = pixels.shape[:2]
    has_alpha = bool(pixels[..., 3].min() < 255)

    head = struct.pack(
        "<II32s32sIIHHBBBB",
        PLATFORM_D3D8, 0x1101,                  # platform, filter/addressing
        name.encode("latin-1", "replace")[:31],
        mask.encode("latin-1", "replace")[:31],
        FMT_8888, int(has_alpha),                # raster format, D3D8 alpha flag
        width, height, 32, 1, RASTER_TEXTURE, 0)  # depth, levels, type, no DXT
    body = head + struct.pack("<I", pixels.nbytes) + pixels.tobytes()
    return _chunk(CHUNK_TEXTURE_NATIVE,
                  _chunk(CHUNK_STRUCT, body, lib) + _chunk(CHUNK_EXTENSION, b"", lib),
                  lib)


def write_txd(path, textures, lib=RW_VERSION_VC):
    """Write a texture dictionary. `textures` is [(name, (h,w,4) RGBA array)].

    Round-trips through `read_txd` in this module, and is a real enough
    RenderWare file for the browser and the matcher to treat what comes out of
    a PSP container exactly like art from any other source.
    """
    items = list(textures)
    body = _chunk(CHUNK_STRUCT, struct.pack("<HH", len(items), 0), lib)
    for entry in items:
        name, image = entry[0], entry[1]
        mask = entry[2] if len(entry) > 2 else ""
        body += texture_native(name, image, mask, lib)
    body += _chunk(CHUNK_EXTENSION, b"", lib)

    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(_chunk(CHUNK_TEXTURE_DICTIONARY, body, lib))
    return path


def write_img(path, files):
    """Pack files into a version-2 IMG archive. `files` is [(name, bytes)].

    Written so a few hundred extracted dictionaries can travel as one file the
    way the games ship them, rather than as a folder someone has to keep
    together. `read_img_directory` reads it straight back.

    Entries are sector-aligned because the format's offsets and sizes are both
    counted in 2048-byte sectors; the directory itself is padded out to a
    sector boundary so the first entry starts on one.
    """
    items = [(name[:23], bytes(data)) for name, data in files]
    header = IMG_MAGIC + struct.pack("<I", len(items))
    table_bytes = len(items) * IMG_ENTRY
    first = -(-(len(header) + table_bytes) // IMG_SECTOR)      # in sectors

    table, blob, sector = b"", b"", first
    for name, data in items:
        span = max(1, -(-len(data) // IMG_SECTOR))
        table += struct.pack("<IHH", sector, span, span)
        table += name.encode("latin-1", "replace").ljust(24, b"\0")
        blob += data.ljust(span * IMG_SECTOR, b"\0")
        sector += span

    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(header + table)
        fh.write(b"\0" * (first * IMG_SECTOR - len(header) - table_bytes))
        fh.write(blob)
    return path
