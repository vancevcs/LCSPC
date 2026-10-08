"""Match source textures to the hash-named textures PPSSPP dumps from LCS.

Ported from the VCS fork's tools/vcs_match.py (gtavcspc 1193f89), the matcher behind
its TXD browser's auto-assign. What it says about signatures, packs, the spread of
candidates across packs and resolution families holds here unchanged; the notes
about families describe VCS, where they were measured, and LCS is the same engine.

PPSSPP keys a replacement on a hash of the texture's data, so a dumped LCS texture
arrives as `<24 hex>.png` with its name nowhere in the file. The same artwork at a
higher resolution is still recognisably itself, so it is found by comparing
pictures: a 16x16 RGB thumbnail, per-channel mean-subtracted, unit length, and one
minus the cosine (0 is identical, past ~0.35 usually unrelated). A cut-out and an
opaque texture are never the same asset, so alpha is a fixed penalty.

What differs from VCS:

- Sources: the VCS fork's sources folder (the PC upscale packs and the Definitive
  Edition dumps) and the Android OBB, which lcsrw lists as the pack "android". One
  index cache per source rather than one per set, so the OBB joins without
  re-reading 120,000 PC textures, and an index whose folder moved is adopted with
  its paths put right rather than rebuilt (`TxdIndex.cached`).
- No aspect penalty. PPSSPP draws a replacement over the original's UV space
  whatever its shape, and the square HD art for a 2:1 PSP texture is that layout on
  a square canvas (docs/lcs/hd-textures.md). For the same reason nothing is
  squeezed to the PSP's shape on install.
- Install keeps the PSP texture's brightness (lcshd.colour_matched: LCS stores its
  textures darker than any HD source), writes DDS (BC1, or BC7 with alpha, one
  level), and puts the entries in textures.ini's manual block through
  lcshd.set_manual, which never touches the hand-made pack's hashes.

    python Tools/lcsmatch.py --dump <TEXTURES/ULUS10041/new> --txd <sources> <main.obb>
"""

import hashlib
import os
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

SIG = 16                    # signature grid, per side
ASPECT_PENALTY = 0.0        # VCS: 0.25. LCS: none, see the header
ALPHA_PENALTY = 0.20        # added when one is cut-out and the other is not
MAX_DISTANCE = 0.45         # past this a candidate is not worth proposing
# Luminance spread (0-255, on a 32x32 grey thumbnail) below which a dumped texture is too
# flat to tell candidates apart: a flat colour or a soft gradient scores close to
# anything. lcshd never binds one on its own (MIN_DETAIL there); here it is never
# pre-picked, and its row says so.
MIN_DETAIL = 6.0


def detail(img):
    """Luminance spread of an image, the way lcshd measures it."""
    img = img.convert("RGBA")
    flat = Image.new("RGBA", img.size, (128, 128, 128, 255))
    flat.alpha_composite(img)
    grey = flat.convert("L").resize((32, 32), Image.LANCZOS)
    return float(np.asarray(grey, np.float32).std())


def signature(img):
    """(vector, aspect, has_alpha) for one PIL image."""
    img = img.convert("RGBA")
    w, h = img.size
    alpha = np.asarray(img.getchannel("A"))
    has_alpha = bool(alpha.min() < 250)

    # Composite onto mid grey so a cut-out's transparent region contributes
    # something stable instead of whatever stale colour sits under the mask.
    flat = Image.new("RGBA", img.size, (128, 128, 128, 255))
    flat.alpha_composite(img)
    small = flat.convert("RGB").resize((SIG, SIG), Image.LANCZOS)

    v = np.asarray(small, np.float32).reshape(-1, 3)
    v -= v.mean(axis=0, keepdims=True)
    v = v.reshape(-1)
    norm = np.linalg.norm(v)
    if norm > 1e-6:
        v /= norm
    return v, (w / h if h else 1.0), has_alpha


def distance(a, b):
    """Compare two (vector, aspect, has_alpha) signatures. Lower is closer."""
    va, aa, ha = a
    vb, ab, hb = b
    d = 1.0 - float(np.dot(va, vb))
    ratio = max(aa, ab) / max(min(aa, ab), 1e-6)
    if ratio > 1.34:                       # more than a third off square
        d += ASPECT_PENALTY * min(ratio - 1.0, 2.0)
    if ha != hb:
        d += ALPHA_PENALTY
    return d


class DumpIndex:
    """Signatures for every PNG in a PPSSPP texture dump, cached to disk."""

    def __init__(self, hashes, vectors, aspects, alphas, sizes, details=None):
        self.hashes = list(hashes)
        self.vectors = vectors             # (n, SIG*SIG*3) float32
        self.aspects = np.asarray(aspects, np.float32)
        self.alphas = np.asarray(alphas, bool)
        self.sizes = list(sizes)
        self.details = np.asarray(details if details is not None
                                  else np.full(len(self.hashes), 255.0), np.float32)

    def flat(self, i):
        """Too flat for a match to mean much (MIN_DETAIL)."""
        return bool(self.details[i] < MIN_DETAIL)

    def __len__(self):
        return len(self.hashes)

    @classmethod
    def build(cls, dump_dir, progress=None):
        files = sorted(f for f in os.listdir(dump_dir) if f.lower().endswith(".png"))
        hashes, vecs, aspects, alphas, sizes, details = [], [], [], [], [], []
        for i, name in enumerate(files):
            path = os.path.join(dump_dir, name)
            try:
                with Image.open(path) as img:
                    size = img.size
                    v, asp, ha = signature(img)
                    spread = detail(img)
            except Exception:
                continue
            hashes.append(os.path.splitext(name)[0])
            vecs.append(v)
            aspects.append(asp)
            alphas.append(ha)
            sizes.append(size)
            details.append(spread)
            if progress and i % 100 == 0:
                progress(i, len(files))
        stack = np.stack(vecs) if vecs else np.zeros((0, SIG * SIG * 3), np.float32)
        return cls(hashes, stack, aspects, alphas, sizes, details)

    def save(self, path):
        np.savez_compressed(
            path, hashes=np.array(self.hashes), vectors=self.vectors,
            aspects=self.aspects, alphas=self.alphas,
            sizes=np.array(self.sizes, np.int32), details=self.details)

    @classmethod
    def load(cls, path):
        z = np.load(path, allow_pickle=False)
        # A cache from before `details` is rebuilt by `cached`, which catches this.
        return cls(list(z["hashes"]), z["vectors"], z["aspects"], z["alphas"],
                   [tuple(s) for s in z["sizes"]], z["details"])

    @classmethod
    def cached(cls, dump_dir, cache_path=None, progress=None, rebuild=False):
        """Load the cache, rebuilding when it is missing or out of date."""
        cache_path = cache_path or os.path.join(dump_dir, ".lcs_match_index.npz")
        if not rebuild and os.path.exists(cache_path):
            try:
                index = cls.load(cache_path)
                fresh = sum(1 for f in os.listdir(dump_dir)
                            if f.lower().endswith(".png"))
                if len(index) == fresh:
                    return index
            except Exception:
                pass
        index = cls.build(dump_dir, progress)
        try:
            index.save(cache_path)
        except OSError:
            pass                            # a read-only dump is still usable
        return index

    def search(self, sig, top=12):
        """Best `top` candidates for one signature, as (hash, size, distance)."""
        if not len(self):
            return []
        v, asp, ha = sig
        d = 1.0 - self.vectors @ v
        ratio = np.maximum(self.aspects, asp) / np.maximum(np.minimum(self.aspects, asp), 1e-6)
        d = d + ASPECT_PENALTY * np.clip(ratio - 1.0, 0, 2.0) * (ratio > 1.34)
        d = d + ALPHA_PENALTY * (self.alphas != ha)
        order = np.argsort(d)[:top]
        return [(self.hashes[i], self.sizes[i], float(d[i])) for i in order]


class TxdIndex:
    """Signatures for every texture under a set of source folders, cached to disk.

    The counterpart to DumpIndex: that one indexes what the PSP drew, this one
    indexes what the source packs have to offer. Holding both as plain matrices
    is what lets the whole cross product be scored in one pass instead of one
    search per texture.

    Entries carry the pack they came from -- the top folder under the scanned
    root -- because with several upscale projects open at once the interesting
    question is usually not "what is closest" but "what does each pack offer for
    this texture", and answering that needs the candidates spread across packs
    rather than six near-duplicates out of whichever archive scored best.
    """

    def __init__(self, sources, offsets, names, dims, vectors, aspects, alphas,
                 containers=None, packs=None):
        self.sources = list(sources)        # .txd / .img / image path per entry
        self.offsets = list(offsets)        # absolute chunk offset, or txd.LOOSE
        self.names = list(names)
        self.dims = list(dims)              # (w, h)
        self.containers = list(containers) if containers is not None \
            else [""] * len(self.names)     # .txd name in an archive, or folder
        self.packs = list(packs) if packs is not None \
            else [""] * len(self.names)     # source pack this came from
        self.vectors = vectors
        self.aspects = np.asarray(aspects, np.float32)
        self.alphas = np.asarray(alphas, bool)

    def __len__(self):
        return len(self.names)

    def info(self, i):
        """A txd.TextureInfo for entry i, so its raster can be reloaded."""
        import lcsrw as txd
        w, h = self.dims[i]
        return txd.TextureInfo(self.names[i], "", w, h, "", 1,
                               bool(self.alphas[i]), self.sources[i],
                               int(self.offsets[i]), 0, self.containers[i])

    def label(self, i):
        """Where entry i lives, archive- or folder-qualified as it needs to be."""
        return self.info(i).label

    def pack(self, i):
        """Which source pack entry i came from, for grouping and captions."""
        return self.packs[i] if i < len(self.packs) else ""

    def pack_counts(self):
        """Every source pack in the index and how many textures it holds."""
        counts = {}
        for name in self.packs:
            counts[name] = counts.get(name, 0) + 1
        return counts

    def only_packs(self, names):
        """This index narrowed to the entries from `names`.

        Asking what one pack alone can offer is the same signatures scored
        against a narrower column set, and those signatures are already
        computed -- so this subsets rather than reindexing. Slicing costs one
        copy of the matrix; rebuilding would mean walking the whole source tree
        again, which over a hundred thousand textures is minutes against
        milliseconds.
        """
        wanted = {n.lower() for n in names}
        keep = [i for i, p in enumerate(self.packs) if p.lower() in wanted]
        if not keep:
            return TxdIndex([], [], [], [],
                            np.zeros((0, SIG * SIG * 3), np.float32), [], [])
        idx = np.asarray(keep, np.intp)
        return TxdIndex([self.sources[i] for i in keep],
                        [self.offsets[i] for i in keep],
                        [self.names[i] for i in keep],
                        [self.dims[i] for i in keep],
                        self.vectors[idx], self.aspects[idx], self.alphas[idx],
                        [self.containers[i] for i in keep],
                        [self.packs[i] for i in keep])

    @staticmethod
    def _pack_of(root, path):
        """The top folder under `root` that `path` sits in.

        Pointing the browser at one folder holding several upscale projects is
        the normal case, so the pack cannot come from the argument list -- it
        has to be recovered per file from where the file actually is.
        """
        import lcsrw as txd
        if txd.is_obb(path):
            return "android"
        rel = os.path.relpath(os.path.abspath(path), root)
        head = rel.replace("\\", "/").split("/")[0]
        return os.path.basename(root) if head in (os.pardir, rel) else head

    @classmethod
    def build_infos(cls, items, progress=None, thumb=64, workers=8):
        """Index a list of (root, TextureInfo): the part of `build` after the walk."""
        def sig_of(item):
            _root, info = item
            try:
                tex = info.load()
                return signature(tex.image(tex.level_for(thumb)))
            except Exception:
                return None

        sources, offsets, names, dims, containers, packs = [], [], [], [], [], []
        vecs, aspects, alphas = [], [], []
        done = 0
        with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
            for start in range(0, len(items), 2048):
                block = items[start:start + 2048]
                for (root, info), sig in zip(block, pool.map(sig_of, block)):
                    done += 1
                    if sig is None:
                        continue
                    v, asp, ha = sig
                    sources.append(info.source)
                    offsets.append(info.offset)
                    names.append(info.name)
                    dims.append((info.width, info.height))
                    containers.append(info.container)
                    packs.append(cls._pack_of(root, info.source))
                    vecs.append(v)
                    aspects.append(asp)
                    alphas.append(ha)
                if progress:
                    progress(done, len(items))
        stack = np.stack(vecs) if vecs else np.zeros((0, SIG * SIG * 3), np.float32)
        return cls(sources, offsets, names, dims, stack, aspects, alphas,
                   containers, packs)

    def top_up(self, root, scanned, progress=None):
        """This index plus signatures for whatever `scanned` lists that it lacks.

        A cache keyed on a path goes stale as files are added under it; the
        scan lists them, so only the new ones are decoded. The adopted VCS
        index was a fortnight older than its folder.
        """
        have = {(os.path.normcase(str(s)), int(o)) for s, o in zip(self.sources, self.offsets)}
        missing = [(root, i) for i in scanned
                   if (os.path.normcase(i.source), int(i.offset)) not in have]
        if not missing:
            return self, 0
        extra = self.build_infos(missing, progress)
        return TxdIndex.concat([self, extra]), len(extra)

    @classmethod
    def build(cls, folders, progress=None, thumb=64, workers=8):
        """Index every texture under one or more folders.

        Loose .txd files, IMG archives (both versions) and standalone image
        files are all walked, which is what lets a San Andreas install be
        pointed at directly -- its textures live inside multi-gigabyte gta3*.img
        archives rather than as separate files -- and equally lets a flat folder
        of upscaled PNGs be used with no dictionary in sight.

        Signatures are computed on a thread pool. Decoding is where all the time
        goes and it parallelises well: the DXT path is numpy, the image path is
        PIL, and both drop the GIL for the expensive part. Over a source tree
        this size that is the difference between minutes and an afternoon.
        """
        import lcsrw as txd
        if isinstance(folders, str):
            folders = [folders]

        items = []
        for folder in folders:
            root = os.path.abspath(folder)
            for info in txd.scan_tree(folder):
                items.append((root, info))

        def sig_of(item):
            _root, info = item
            try:
                tex = info.load()
                return signature(tex.image(tex.level_for(thumb)))
            except Exception:
                return None

        sources, offsets, names, dims, containers, packs = [], [], [], [], [], []
        vecs, aspects, alphas = [], [], []
        done = 0
        with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
            # Submitted in blocks rather than all at once: one live future per
            # texture over a tree this large is a lot of objects for no gain,
            # and finishing a block before starting the next keeps progress
            # honest instead of reporting everything as queued immediately.
            for start in range(0, len(items), 2048):
                block = items[start:start + 2048]
                for (root, info), sig in zip(block, pool.map(sig_of, block)):
                    done += 1
                    if sig is None:
                        continue
                    v, asp, ha = sig
                    sources.append(info.source)
                    offsets.append(info.offset)
                    names.append(info.name)
                    dims.append((info.width, info.height))
                    containers.append(info.container)
                    packs.append(cls._pack_of(root, info.source))
                    vecs.append(v)
                    aspects.append(asp)
                    alphas.append(ha)
                if progress:
                    progress(done, len(items))
        if progress:
            progress(len(items), len(items))
        stack = np.stack(vecs) if vecs else np.zeros((0, SIG * SIG * 3), np.float32)
        return cls(sources, offsets, names, dims, stack, aspects, alphas,
                   containers, packs)

    def save(self, path):
        np.savez_compressed(
            path, sources=np.array(self.sources),
            offsets=np.array(self.offsets, np.int64),
            names=np.array(self.names), dims=np.array(self.dims, np.int32),
            containers=np.array(self.containers), packs=np.array(self.packs),
            vectors=self.vectors, aspects=self.aspects, alphas=self.alphas)

    @classmethod
    def load(cls, path):
        z = np.load(path, allow_pickle=False)

        def optional(key):
            return list(z[key]) if key in z.files else None

        return cls(list(z["sources"]), list(z["offsets"]), list(z["names"]),
                   [tuple(d) for d in z["dims"]], z["vectors"], z["aspects"],
                   z["alphas"], optional("containers"), optional("packs"))

    @staticmethod
    def cache_path(folder):
        """Where one source's index lives: beside it, named by a hash of its path.

        The name the VCS tool gives the index of a one-folder set, so the two tools
        share the VCS fork's sources index.
        """
        folder = os.path.abspath(folder)
        stamp = hashlib.blake2b(folder.encode(), digest_size=8).hexdigest()
        where = folder if os.path.isdir(folder) else os.path.dirname(folder)
        return os.path.join(where, f".txd_index_{stamp}.npz")

    @classmethod
    def _adopt(cls, folder, cache_path):
        """An index built before its folder moved, with its paths put right.

        The VCS fork's sources were indexed in Desktop\\sources and then moved to
        Dev\\gtavcspc\\sources: the cache moved with them under the old path's name, and
        every path inside it points at the old place. The signatures are still good,
        and rebuilding them is the better part of an hour. An index is taken when the
        part of its first path before its pack's folder can be swapped for `folder`
        and a sample of the swapped paths all exist; it is saved under the new name.
        """
        where = os.path.dirname(cache_path)
        for name in sorted(os.listdir(where)):
            other = os.path.join(where, name)
            if (not (name.startswith(".txd_index_") and name.endswith(".npz"))
                    or os.path.normcase(other) == os.path.normcase(cache_path)):
                continue
            try:
                index = cls.load(other)
            except Exception:
                continue
            if not len(index) or not index.packs[0]:
                continue
            first = str(index.sources[0])
            at = first.find(os.sep + str(index.packs[0]) + os.sep)
            if at < 0:
                continue
            old_root = first[:at]
            if os.path.normcase(old_root) == os.path.normcase(folder):
                continue
            moved = [folder + str(p)[len(old_root):] if str(p).startswith(old_root) else str(p)
                     for p in index.sources]
            step = max(1, len(moved) // 64)
            if not all(os.path.exists(p) for p in moved[::step]):
                continue
            index.sources = moved
            try:
                index.save(cache_path)
            except OSError:
                pass
            return index
        return None

    @classmethod
    def concat(cls, parts):
        """Several indexes as one, in order."""
        parts = [p for p in parts if len(p)]
        if not parts:
            return cls([], [], [], [], np.zeros((0, SIG * SIG * 3), np.float32), [], [])
        if len(parts) == 1:
            return parts[0]

        def join(attr):
            return [x for p in parts for x in getattr(p, attr)]

        return cls(join("sources"), join("offsets"), join("names"), join("dims"),
                   np.concatenate([p.vectors for p in parts]),
                   np.concatenate([p.aspects for p in parts]),
                   np.concatenate([p.alphas for p in parts]),
                   join("containers"), join("packs"))

    @classmethod
    def cached(cls, folders, cache_path=None, progress=None, rebuild=False, scans=None):
        """Each source's cached index, building the missing ones, joined.

        Keyed on a source's path, not its contents: walking a hundred thousand
        textures to check costs what the cache saves. `scans` ({folder: [TextureInfo]},
        what the browser already listed) tops a cache up with the textures it lacks
        (`top_up`); without it, rebuild after adding to a source (the browser's
        Reindex). `cache_path` is accepted for the VCS signature and ignored.
        """
        scans = {os.path.normcase(os.path.abspath(k)): v for k, v in (scans or {}).items()}
        if isinstance(folders, str):
            folders = [folders]
        parts = []
        for folder in folders:
            folder = os.path.abspath(folder)
            path = cls.cache_path(folder)
            index = None
            if not rebuild and os.path.exists(path):
                try:
                    index = cls.load(path)
                except Exception:
                    index = None
            if index is None and not rebuild and os.path.isdir(folder):
                index = cls._adopt(folder, path)
            if index is None or not len(index):
                index = cls.build([folder], progress)
                try:
                    index.save(path)
                except OSError:
                    pass
            elif os.path.normcase(folder) in scans:
                index, added = index.top_up(folder, scans[os.path.normcase(folder)], progress)
                if added:
                    try:
                        index.save(path)
                    except OSError:
                        pass
            parts.append(index)
        return cls.concat(parts)


def _spread(cols, dists, txds, top, per_pack):
    """Pick `top` candidates from a scored shortlist, spread across packs.

    Straight top-N over a large multi-pack source tree returns the same artwork
    over and over -- one row of ten was measured as ten copies of a single
    texture -- which fills a row of alternatives with one choice wearing ten
    hats. Two things thin that out.

    Exact repeats are dropped: same texture name, same pack, same dimensions is
    the same file reached twice, once loose and once inside an archive, and only
    the closer of the two is worth a slot. Note the pack and the size are part
    of that key on purpose. The same texture at 128 and at 1024, or at 512 from
    two different upscale projects, is a real choice between different pictures
    and both stay.

    What survives is then capped at `per_pack` from any one pack, so a project
    that happens to score well cannot crowd the others out. Anything displaced
    is kept in reserve and backfilled, so this never returns fewer candidates
    than the plain ranking would have.
    """
    if not txds:
        return list(zip(cols, dists))[:top]

    names, dims = txds.names, txds.dims
    picked, spare, seen, already = [], [], {}, set()
    for col, dist in zip(cols, dists):
        pack = txds.pack(col)
        key = (names[col].lower(), pack, dims[col])
        if key in already:
            continue
        already.add(key)
        if per_pack <= 0 or seen.get(pack, 0) < per_pack:
            seen[pack] = seen.get(pack, 0) + 1
            picked.append((col, dist))
        else:
            spare.append((col, dist))
        if len(picked) == top:
            return picked
    return (picked + spare)[:top]


def assign(dump, txds, top=8, max_distance=MAX_DISTANCE, chunk=64, progress=None,
           per_pack=3, pool=6):
    """Best source candidates for every dumped LCS texture.

    Returns [(dump_index, [(txd_index, distance), ...])], one row per dumped
    texture that found anything within `max_distance`, ordered best first. Each
    row keeps up to `top` candidates so they can be reviewed side by side, with
    duplicates removed and no more than `per_pack` of them drawn from any single
    source pack -- see `_spread`, which is what makes a row of ten a row of ten
    real options rather than one texture repeated.

    Scored as one matrix product per chunk of dumped textures rather than a
    search per texture: the cross product here runs to hundreds of millions of
    pairs, and doing it row by row would take hours where BLAS takes seconds.
    The chunking keeps the intermediate distance matrix small -- at this many
    source textures a chunk of 64 is already tens of megabytes per temporary,
    and everything stays float32 so a stray Python float cannot silently double
    that.
    """
    out = []
    if not len(dump) or not len(txds):
        return out

    tv = txds.vectors
    ta = txds.aspects[None, :]
    tal = txds.alphas[None, :]
    keep = min(max(top * pool, top), len(txds))

    for start in range(0, len(dump), chunk):
        stop = min(start + chunk, len(dump))
        d = dump.vectors[start:stop] @ tv.T
        np.negative(d, out=d)
        d += np.float32(1.0)

        da = dump.aspects[start:stop, None]
        ratio = np.maximum(da, ta) / np.maximum(np.minimum(da, ta), 1e-6)
        d += ASPECT_PENALTY * np.clip(ratio - 1.0, 0, 2.0) * (ratio > 1.34)
        d += ALPHA_PENALTY * (dump.alphas[start:stop, None] != tal)

        part = np.argpartition(d, keep - 1, axis=1)[:, :keep]
        for row in range(stop - start):
            cols = part[row]
            cols = cols[np.argsort(d[row, cols])]
            near = [c for c in cols if d[row, c] <= max_distance]
            if not near:
                continue
            hits = _spread(near, [float(d[row, c]) for c in near],
                           txds, top, per_pack)
            out.append((start + row, [(int(c), float(x)) for c, x in hits]))
        if progress:
            progress(stop, len(dump))

    out.sort(key=lambda r: r[1][0][1])
    return out


def leftovers(rows, dump, installed=(), limit=MAX_DISTANCE):
    """The dumped textures nothing was really found for.

    Half a pack in, the interesting question stops being "what should replace
    this" and becomes "what has nothing at all". That is two populations at
    once: hashes already carrying a replacement, and hashes some candidate came
    close enough to that the ordinary review is already offering them. What
    neither covers is the rest, and on a dump this size the rest is most of it.

    `rows` is assign() run with no distance limit, so every dumped texture has
    a row and this is a split rather than a second search. What comes back has
    the same shape assign returns -- candidates and all -- because a hopeless
    row is still worth showing its nearest miss: at this distance the search is
    guessing, and a guess a person can glance at beats a blank line. Least
    hopeless first, so the salvageable ones are at the top.
    """
    installed = set(installed)
    return [(d, cands) for d, cands in rows
            if cands and cands[0][1] >= limit and dump.hashes[d] not in installed]


# ---------------------------------------------------- resolution families

# Vice City Stories draws a distant building from a different model with its
# own, smaller texture rather than from a mip of the near one, so the dump
# holds the same artwork several times over at 128, 64, 32 and down. PPSSPP is
# told to ignore mipmaps, which means every dumped PNG is a level 0 texture the
# game actually bound -- the ladder in the dump is the game's own LOD art, not
# a mip chain, and each rung has its own hash and needs its own replacement.
#
# Grading them apart is what makes an upscale pack worth having: the 8x version
# belongs on the rung the player stands next to, and spending it on the 16x16
# the game draws three blocks away buys nothing but memory.

FAMILY_SIG_GATE = 0.12      # signature shortlist, before anything is decoded
FAMILY_RESIDUAL = 0.09      # a smaller texture is the same art, downscaled
SIBLING_RESIDUAL = 0.03     # same size, another hash: the same picture twice


def _rung(big, small):
    """How many times `small` halves out of `big`, or None if it does not.

    PSP textures are powers of two and the game's smaller variants are honest
    halvings of the larger, both axes at once. Demanding that up front throws
    away most of the shortlist for free -- a 64x64 and a 48x16 are not two
    rungs of anything however alike they look -- and saves the pixel check,
    which is the part that costs, for pairs that could really be related.
    """
    bw, bh = big
    sw, sh = small
    if sw > bw or sh > bh or bw % sw or bh % sh:
        return None
    ratio = bw // sw
    return ratio if ratio == bh // sh and not ratio & (ratio - 1) else None


def _grey(img):
    """RGB as float, cut-outs composited onto mid grey.

    The same treatment `signature` gives them, and for the same reason: what
    sits underneath a mask is whatever colour was left there, and comparing it
    scores two identical cut-outs as different pictures.
    """
    flat = Image.new("RGBA", img.size, (128, 128, 128, 255))
    flat.alpha_composite(img)
    return np.asarray(flat.convert("RGB"), np.float32)


def _residual(big, small):
    """Mean absolute error, 0..1, of `big` rescaled onto `small`.

    The signature is deliberately blunt -- 16x16, mean removed -- because it
    has to survive a PC texture and a PSP one being the same artwork through
    different palettes. That blur is exactly wrong here: two neighbouring
    windows on one building wall sit a hair apart at 16x16 and are obviously
    different pictures at full size. Rescaling one onto the other and taking
    the error straight is what separates a real rung from a near neighbour.
    """
    if big.size != small.size:
        big = big.resize(small.size, Image.LANCZOS)
    return float(np.abs(_grey(big) - _grey(small)).mean() / 255.0)


class Families:
    """Dumped textures grouped into the same artwork at several resolutions.

    A family is one picture as the game ships it at every size it draws it --
    the 128x128 on the building you are standing at, the 64x64 a street away,
    the 16x16 on the skyline -- plus any same-size repeats, which are usually
    one texture recoloured per character or per time of day.

    Members are ordered largest first, because that is the order the choice is
    made in: the big rung gets the good upscale and the rest are graded down
    from it. Keyed by hash rather than by index, so the grouping survives being
    handed a query set built from extracted art, which numbers its rows
    differently but names them the same.
    """

    def __init__(self, hashes, ids, sizes):
        self.sizes = {h: tuple(s) for h, s in zip(hashes, sizes)}
        self.of = {h: int(f) for h, f in zip(hashes, ids) if f >= 0}
        groups = {}
        for h, fid in self.of.items():
            groups.setdefault(fid, []).append(h)
        self.members = {
            fid: sorted(m, key=lambda h: (-self.sizes[h][0] * self.sizes[h][1], h))
            for fid, m in groups.items()}
        self._hashes = list(hashes)
        self._ids = np.asarray(ids, np.int64)

    def __len__(self):
        return len(self.members)

    def id(self, hash_name):
        """Which family a hash belongs to, or None when it stands alone."""
        return self.of.get(hash_name)

    def kin(self, hash_name):
        """Every hash in this one's family, largest first; empty when alone."""
        fid = self.of.get(hash_name)
        return self.members.get(fid, ()) if fid is not None else ()

    def rank(self, hash_name):
        """(position, total) within the family, 1-based and largest first."""
        kin = self.kin(hash_name)
        return (kin.index(hash_name) + 1, len(kin)) if kin else (1, 1)

    def ladder(self, fid):
        """The family's distinct sizes as text, largest first."""
        seen = []
        for h in self.members.get(fid, ()):
            if self.sizes[h] not in seen:
                seen.append(self.sizes[h])
        return " · ".join(f"{w}×{h}" for w, h in seen)

    # ------------------------------------------------------------- building

    @classmethod
    def build(cls, dump, dump_dir, progress=None, chunk=256):
        """Find the families in a dump: shortlist by signature, confirm by pixels.

        The shortlist is the same matrix product `assign` uses, run against the
        dump itself, and it is only a shortlist -- everything it offers is then
        rescaled and compared properly. Only what reaches that stage is ever
        decoded, which over a dump this size is a fifth of the files.

        A smaller texture keeps its single best parent rather than every parent
        it could pass, so a wall that matches three of its own neighbours joins
        one ladder instead of welding all four into a lump.
        """
        n = len(dump)
        ids = np.full(n, -1, np.int64)
        sizes = [tuple(s) for s in dump.sizes]
        if n < 2:
            return cls(dump.hashes, ids, sizes)

        down, same = [], []
        for start in range(0, n, chunk):
            stop = min(start + chunk, n)
            d = dump.vectors[start:stop] @ dump.vectors.T
            np.negative(d, out=d)
            d += np.float32(1.0)
            for r, c in zip(*np.where(d < FAMILY_SIG_GATE)):
                a, b = start + int(r), int(c)
                if a == b:
                    continue
                ratio = _rung(sizes[a], sizes[b])
                if ratio is None:
                    continue
                # Every unordered pair is looked at once, bigger first. A
                # same-size pair has no bigger, so it is taken on a < b.
                if ratio > 1:
                    down.append((a, b))
                elif a < b:
                    same.append((a, b))
            if progress:
                progress(stop, n)

        cache = {}

        def image(i):
            if i not in cache:
                path = os.path.join(dump_dir, dump.hashes[i] + ".png")
                with Image.open(path) as src:
                    cache[i] = src.convert("RGBA")
            return cache[i]

        total = n + len(down) + len(same)
        links, parent, done = [], {}, 0
        for pairs, limit, ladder in ((down, FAMILY_RESIDUAL, True),
                                     (same, SIBLING_RESIDUAL, False)):
            for a, b in pairs:
                done += 1
                if progress and done % 200 == 0:
                    progress(n + done, total)
                try:
                    error = _residual(image(a), image(b))
                except Exception:
                    continue                    # a PNG that will not open
                if error >= limit:
                    continue
                if not ladder:
                    links.append((a, b))
                elif error < parent.get(b, (0, 9.0))[1]:
                    parent[b] = (a, error)
        links += [(a, b) for b, (a, _e) in parent.items()]

        # Union-find over what survived. The ladders are already trees -- one
        # parent each -- so this is really about folding the same-size repeats
        # in alongside whichever rung they matched.
        root = list(range(n))

        def find(x):
            while root[x] != x:
                root[x] = root[root[x]]
                x = root[x]
            return x

        for a, b in links:
            ra, rb = find(a), find(b)
            if ra != rb:
                root[ra] = rb

        grouped = {}
        for i in range(n):
            grouped.setdefault(find(i), []).append(i)
        # Numbered by their first member so a rebuild of the same dump gives
        # the same ids, which is what makes the cache comparable at all.
        fid = 0
        for members in sorted(grouped.values(), key=lambda m: m[0]):
            if len(members) < 2:
                continue
            for i in members:
                ids[i] = fid
            fid += 1
        if progress:
            progress(total, total)
        return cls(dump.hashes, ids, sizes)

    def save(self, path):
        np.savez_compressed(
            path, hashes=np.array(self._hashes), ids=self._ids,
            sizes=np.array([self.sizes[h] for h in self._hashes], np.int32))

    @classmethod
    def load(cls, path):
        z = np.load(path, allow_pickle=False)
        return cls(list(z["hashes"]), z["ids"], [tuple(s) for s in z["sizes"]])

    @classmethod
    def cached(cls, dump, dump_dir, cache_path=None, progress=None, rebuild=False):
        """Load the cache, rebuilding when the dump behind it has changed."""
        cache_path = cache_path or os.path.join(dump_dir, ".lcs_families.npz")
        if not rebuild and os.path.exists(cache_path):
            try:
                found = cls.load(cache_path)
                if found._hashes == list(dump.hashes):
                    return found
            except Exception:
                pass
        found = cls.build(dump, dump_dir, progress)
        try:
            found.save(cache_path)
        except OSError:
            pass                                # a read-only dump is still usable
        return found


# ------------------------------------------------------------------- install

def _lcshd():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import lcshd
    return lcshd


def pack_folder(pack):
    """A source pack's name as a folder in the PPSSPP pack: lower case, no spaces.

    Lower case on purpose: PPSSPP flags a capital in an ini path as a possibly
    non-portable file name (the VCS fork's install notes the same).
    """
    return re.sub(r"[^a-z0-9_.-]+", "_", pack.lower()).strip("_") or "pack"


def install_path(hash_name, pack, name="", container=""):
    """Where a replacement goes, relative to the PPSSPP pack, as a PNG.

    Android art keeps lcstxdweb's layout, android/<txd>/<texture>~<last 8 of the
    hash>, so both tools read it back as that texture's binding; the PC packs go to
    pc/<pack>/<hash>. The DDS replaces the .png once it is converted.
    """
    if pack == "android":
        return "android/%s/%s~%s.png" % (container.lower(), name.lower(), hash_name[-8:])
    return "pc/%s/%s.png" % (pack_folder(pack), hash_name)


def install_many(jobs, pack_dir, dump_dir, colour=True, dds=True, workers=8,
                 progress=None, cancelled=None):
    """Install a batch of replacements. `jobs` is [(hash, load, rel, comment)].

    `load` returns the source image (decoded a few at a time, not all held at
    once), `rel` is where it goes (install_path). Each is colour-matched to the
    dumped texture it replaces when `colour` is on, written as PNG, and converted
    to DDS when `dds` is on and texconv is installed. textures.ini is written once
    at the end, into the manual block; a hash the hand-made pack holds is refused
    and reported, and a file an earlier pick wrote that nothing names any more is
    deleted.

    Returns ({hash: rel written}, failures).
    """
    lcshd = _lcshd()
    texconv = lcshd.find_texconv() if dds else None
    refused = lcshd.handmade(pack_dir)
    jobs = list(jobs)
    done, comments, failures = {}, {}, []
    lock = threading.Lock()

    def one(job):
        hash_name, load, rel, comment = job
        if cancelled and cancelled():
            return
        if hash_name.lower() in refused:
            with lock:
                failures.append(f"{hash_name}: the hand-made pack has this hash; change "
                                f"it there and re-run lcshd merge")
            return
        try:
            image = load().convert("RGBA")
            if colour:
                with Image.open(os.path.join(dump_dir, hash_name + ".png")) as dump:
                    image, _gains = lcshd.colour_matched(image, dump)
            path = os.path.join(pack_dir, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            image.save(path)
            if texconv and lcshd.png_to_dds(texconv, path):
                rel = rel[:-4] + ".dds"
        except Exception as exc:
            with lock:
                failures.append(f"{hash_name}: {exc}")
            return
        with lock:
            done[hash_name] = rel
            comments[hash_name] = comment
            if progress:
                progress(len(done) + len(failures), len(jobs))

    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        list(pool.map(one, jobs))

    if done:
        old, _refused = lcshd.set_manual(pack_dir, done, comments)
        lcshd.drop_unused(pack_dir, [v for k, v in old.items() if v and v != done.get(k)])
    return done, failures


# ----------------------------------------------------------------------- cli

def main():
    import argparse
    import csv
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dump", required=True, help="PPSSPP dump folder of <hash>.png")
    ap.add_argument("--txd", required=True, nargs="+", metavar="FOLDER",
                    help="source folder(s): .txd files, IMG archives, or images")
    ap.add_argument("-o", "--out", default="matches.csv")
    ap.add_argument("--max-distance", type=float, default=0.18,
                    help="only report matches at least this close")
    ap.add_argument("--top", type=int, default=8,
                    help="alternatives kept per dumped texture")
    ap.add_argument("--per-pack", type=int, default=3,
                    help="most alternatives from any one source pack (0 = no limit)")
    ap.add_argument("--rebuild", action="store_true", help="ignore the cached indexes")
    args = ap.parse_args()

    def tick(label):
        return lambda i, n: print(f"\r    {label} {i:,}/{n:,}", end="", flush=True)

    print(f"  indexing dump: {args.dump}")
    dump = DumpIndex.cached(args.dump, rebuild=args.rebuild,
                            progress=tick("dumped"))
    print(f"\r    {len(dump):,} dumped textures indexed        ")

    print(f"  indexing sources: {', '.join(args.txd)}")
    txds = TxdIndex.cached(args.txd, rebuild=args.rebuild,
                           progress=tick("source"))
    print(f"\r    {len(txds):,} source textures indexed        ")

    rows = []
    for dump_idx, cands in assign(dump, txds, top=args.top,
                                  max_distance=args.max_distance,
                                  per_pack=args.per_pack,
                                  progress=tick("scoring")):
        for rank, (i, dist) in enumerate(cands, 1):
            rows.append({
                "psp_hash": dump.hashes[dump_idx],
                "psp_w": dump.sizes[dump_idx][0], "psp_h": dump.sizes[dump_idx][1],
                "rank": rank, "distance": round(dist, 4),
                "texture": txds.names[i], "pack": txds.pack(i),
                "source": txds.label(i),
                "w": txds.dims[i][0], "h": txds.dims[i][1],
            })
    print(f"\r    {len(rows):,} candidate(s) over "
          f"{len({r['psp_hash'] for r in rows}):,} dumped textures        ")

    fields = ["psp_hash", "psp_w", "psp_h", "rank", "distance", "texture",
              "pack", "source", "w", "h"]
    with open(args.out, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nwrote {len(rows)} candidate match(es) to {os.path.abspath(args.out)}")
    print("  Rank 1 is the closest; the rest are the alternatives the browser "
          "shows side by side.")
    print("  Review before installing -- a low distance is a strong hint, not proof.")


if __name__ == "__main__":
    main()
