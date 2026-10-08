"""HD textures for LCS from the Android build, bound to PPSSPP's texture hashes by similarity.

PPSSPP names a dumped texture by a hash of its data; the Android build names its HD art by the PSP
texture's name. Nothing on either side shares a key, so the two are joined by comparing pictures:
the Android art is LCS's own art at four to eight times the size, so a dumped PSP raster and its
HD version look alike once both are shrunk to a thumbnail.

    python Tools/lcshd.py index   --obb <main.obb> [--patch <patch.obb>]
    python Tools/lcshd.py match   --dump <memstick>/PSP/TEXTURES/ULUS10041/new
    python Tools/lcshd.py review  [--tier sure|likely|review]
    python Tools/lcshd.py bind    --pack <memstick>/PSP/TEXTURES/ULUS10041 --dump <...>/new [--tier sure|likely]
    python Tools/lcshd.py dds     --pack <memstick>/PSP/TEXTURES/ULUS10041
    python Tools/lcshd.py merge   --pack <memstick>/PSP/TEXTURES/ULUS10041 [--from <hand-made pack>]

Work files go to --work (default: hd/ in the workspace, beside ppsspp/): the Android index
(android.npz), the dump index (dump.npz, rebuilt when the dump grows), matches.csv and the review
sheets.

index   decodes every HD texture (lcswad.py has the container and the PVR formats) and keeps a
        signature of each: a 16x16 RGB thumbnail, mean-subtracted per channel and normalised, so it
        compares pattern rather than brightness - the PSP's palettes and the Android art's grading
        differ, the layout does not. This is the VCS fork's vcs_match signature, minus its aspect
        penalty: the Android art is square where a PSP texture can be 2:1, and PPSSPP maps a
        replacement over the original's UV space whatever its shape.
match   scores every dumped texture against every HD texture (one matrix product) and writes
        matches.csv: the best HD texture, its distance (0 is identical), the margin to the best
        *different* picture (the Android set has the same art in several TXDs), and a tier.
review  a contact sheet per tier: the dump, the HD texture, the distance. Look before binding.
bind    writes the matched HD textures into the pack as PNG, and their hashes into textures.ini.
        A hash the ini already has is left alone, so a hand-made texture (the HUD pack) wins.
        Each HD texture is given the average colour of the PSP texture it replaces, per channel:
        the PSP's are stored darker than the Android art (median 0.62 of its brightness on the
        first dump - half-intensity textures the GE doubles, and the mobile port's own grading),
        so unadjusted art comes out washed out. Matching the mean keeps the PSP's lighting and
        adds only the detail.
dds     converts the pack's bound PNGs with texconv: BC1 for opaque art, BC7 for art with alpha,
        one level (mipmaps would hand back the PSP's resolution), and points the ini at them.
        Only android/ is converted: block compression smears text and thin UI lines.
merge   brings a hand-made pack in (default: texturereplacements/ beside ppsspp/, the HD UI and HUD
        pack): its files are copied beside android/ and its hashes go in as one marked block that
        wins over any Android binding of the same hash. Re-run it after changing that pack.
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcswad  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = os.path.dirname(os.path.dirname(HERE))
DEFAULT_WORK = os.path.join(WORKSPACE, 'hd')
DEFAULT_OBB = os.path.join(WORKSPACE, 'com.rockstargames.gtalcs', 'main.17.com.rockstargames.gtalcs.obb')
DEFAULT_PATCH = os.path.join(WORKSPACE, 'com.rockstargames.gtalcs', 'patch.15.com.rockstargames.gtalcs.obb')

SIG = 16
FINE = 32                 # the second look at the best candidates
ALPHA_PENALTY = 0.20      # a cut-out and an opaque texture are never the same asset
COLOUR_WEIGHT = 0.50      # the HD art keeps the PSP texture's colours; a dark and a white
                          # gradient have the same pattern and are not the same texture
SAME_PICTURE = 0.06       # two HD textures this close are one picture filed twice (or resized,
                          # or recoloured): the margin is measured past them
MIN_DUMP_SIZE = 16        # smaller dumps are flat colours and gradients: any match is a guess
MIN_DETAIL = 6.0          # luminance std below this (0-255): too flat to tell apart, never auto
GAIN_LIMITS = (0.25, 2.0) # how far the colour match may scale a channel
GAIN_SAME = 0.04          # one HD texture bound at gains this close shares one file
# (name, max distance, min margin). The margin decides more than the distance does: on the first
# park-and-drive dump (867 textures) every wrong pair seen in the sure tier was within 0.10 of a
# different picture, and right ones mostly far clear. Bind 'sure'; look at 'likely' first.
TIERS = (('sure', 0.16, 0.12), ('likely', 0.22, 0.05))
PACK_SUBDIR = 'android'


def _unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-6 else v


def signature(rgba):
    """(vector, has_alpha, mean colour 0-1, luminance std, fine grey vector) for (h, w, 4) uint8."""
    img = Image.fromarray(rgba, 'RGBA')
    has_alpha = bool(rgba[..., 3].min() < 250)
    flat = Image.new('RGBA', img.size, (128, 128, 128, 255))
    flat.alpha_composite(img)
    rgb = flat.convert('RGB')
    v = np.asarray(rgb.resize((SIG, SIG), Image.LANCZOS), np.float32).reshape(-1, 3)
    mean = v.mean(axis=0) / 255.0
    v -= v.mean(axis=0, keepdims=True)
    g = np.asarray(rgb.convert('L').resize((FINE, FINE), Image.LANCZOS), np.float32).reshape(-1)
    std = float(g.std())
    return _unit(v.reshape(-1)), has_alpha, mean, std, _unit(g - g.mean())


# --- index ---

_ARCH = None


def _worker_init(obb, patch):
    global _ARCH
    _ARCH = lcswad.Archives(obb, patch)


def _index_one(h):
    w, e = _ARCH.by_hash[h]
    rgba, hd = lcswad.decode_pvr(w.read(e))
    return (h, hd['width'], hd['height']) + signature(rgba)


def cmd_index(args):
    arch = lcswad.Archives(args.obb, args.patch if os.path.exists(args.patch or '') else None)
    textures = [h for h, _ in lcswad.hd_textures(arch)]
    names = lcswad.hd_texture_names(arch)
    nodes = {h: arch.by_hash[h][1]['node'] for h in textures}
    print('%d HD textures, %d named; decoding' % (len(textures), sum(1 for h in textures if h in names)))
    rows = []
    with ProcessPoolExecutor(initializer=_worker_init, initargs=(args.obb, arch.wads[1].path if len(arch.wads) > 1 else None)) as pool:
        for i, r in enumerate(pool.map(_index_one, textures, chunksize=32)):
            rows.append(r)
            if i % 1000 == 0:
                print('  %d / %d' % (i, len(textures)), flush=True)
    hashes = np.array([r[0] for r in rows], np.uint32)
    keys = np.array([names.get(r[0], 'folder%04d/%08x' % (nodes[r[0]], r[0])) for r in rows])
    os.makedirs(args.work, exist_ok=True)
    np.savez_compressed(os.path.join(args.work, 'android.npz'), hashes=hashes, keys=keys,
                        sizes=np.array([(r[1], r[2]) for r in rows], np.int32),
                        vectors=np.stack([r[3] for r in rows]), alphas=np.array([r[4] for r in rows]),
                        means=np.stack([r[5] for r in rows]), stds=np.array([r[6] for r in rows], np.float32),
                        fine=np.stack([r[7] for r in rows]),
                        obb=np.array([args.obb]), patch=np.array([args.patch or '']))
    print('wrote %s' % os.path.join(args.work, 'android.npz'))


def load_android(work):
    z = np.load(os.path.join(work, 'android.npz'))
    return {k: z[k] for k in z.files}


# --- dump ---

def _dump_one(path):
    with Image.open(path) as img:
        rgba = np.asarray(img.convert('RGBA'))
    return (os.path.splitext(os.path.basename(path))[0], (rgba.shape[1], rgba.shape[0])) + signature(rgba)


DUMP_FIELDS = ('sizes', 'vectors', 'alphas', 'means', 'stds', 'fine')


def load_dump(dump_dir, work):
    """Signatures of every PNG in a PPSSPP dump folder, cached and topped up as the dump grows:
    {hash: (size, vector, alpha, mean, std, fine)}."""
    cache = os.path.join(work, 'dump.npz')
    have = {}
    if os.path.exists(cache):
        z = np.load(cache)
        if all(k in z.files for k in DUMP_FIELDS):
            for i, h in enumerate(z['hashes']):
                have[str(h)] = tuple(z[k][i] for k in DUMP_FIELDS)
    files = [os.path.join(dump_dir, f) for f in os.listdir(dump_dir) if f.lower().endswith('.png')]
    todo = [f for f in files if os.path.splitext(os.path.basename(f))[0] not in have]
    if todo:
        with ProcessPoolExecutor() as pool:
            for r in pool.map(_dump_one, todo, chunksize=32):
                have[r[0]] = r[1:]
        os.makedirs(work, exist_ok=True)
        hs = sorted(have)
        np.savez_compressed(cache, hashes=np.array(hs),
                            **{k: np.array([have[h][i] for h in hs]) for i, k in enumerate(DUMP_FIELDS)})
    present = {os.path.splitext(os.path.basename(f))[0] for f in files}
    return {h: have[h] for h in sorted(present)}


# --- match ---

def tier_of(distance, margin):
    for name, dmax, mmin in TIERS:
        if distance <= dmax and margin >= mmin:
            return name
    return 'review'


def cmd_match(args):
    android = load_android(args.work)
    dump = load_dump(args.dump, args.work)
    av, aa, am, af = android['vectors'], android['alphas'], android['means'], android['fine']
    rows = []
    for h, ((w, ht), v, alpha, mean, std, fine) in dump.items():
        if min(w, ht) < MIN_DUMP_SIZE:
            continue
        colour = COLOUR_WEIGHT * np.linalg.norm(am - mean, axis=1) / np.sqrt(3.0)
        coarse = 1.0 - av @ v + ALPHA_PENALTY * (aa != alpha) + colour
        order = np.argsort(coarse)[:24]
        # The second look: the same comparison on a finer grey grid, averaged in.
        d = {int(j): 0.5 * (coarse[j] + (1.0 - float(af[j] @ fine)) + ALPHA_PENALTY * (aa[j] != alpha) + colour[j])
             for j in order}
        ranked = sorted(d, key=d.get)
        best = ranked[0]
        # The margin is to the best candidate that is a different picture, not the same art filed
        # in another TXD.
        others = [j for j in ranked[1:]
                  if 0.5 * ((1.0 - float(av[j] @ av[best])) + (1.0 - float(af[j] @ af[best]))) > SAME_PICTURE]
        margin = float(d[others[0]] - d[best]) if others else 1.0
        tier = tier_of(d[best], margin) if std >= MIN_DETAIL else 'review'
        rows.append(dict(dump=h, dump_size='%dx%d' % (w, ht), android=str(android['keys'][best]),
                         android_hash='%08x' % android['hashes'][best],
                         android_size='%dx%d' % tuple(android['sizes'][best]),
                         distance='%.4f' % d[best], margin='%.4f' % margin, detail='%.1f' % std, tier=tier))
    rows.sort(key=lambda r: float(r['distance']))
    out = os.path.join(args.work, 'matches.csv')
    with open(out, 'w', newline='') as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ['dump'])
        wr.writeheader()
        wr.writerows(rows)
    counts = {}
    for r in rows:
        counts[r['tier']] = counts.get(r['tier'], 0) + 1
    print('%d dumped textures scored (%d too small to judge): %s' % (len(rows), len(dump) - len(rows), counts))
    print('wrote %s' % out)


def read_matches(work):
    with open(os.path.join(work, 'matches.csv'), newline='') as f:
        return list(csv.DictReader(f))


# --- review ---

def _android_image(arch, hash_hex):
    w, e = arch.by_hash[int(hash_hex, 16)]
    rgba, _ = lcswad.decode_pvr(w.read(e))
    return Image.fromarray(rgba, 'RGBA')


def _open_arch(work):
    a = load_android(work)
    patch = str(a['patch'][0])
    return lcswad.Archives(str(a['obb'][0]), patch if patch and os.path.exists(patch) else None)


def _on_checker(img, cell):
    """A thumbnail over a checkerboard, so a cut-out shows as one: dropping the alpha makes a
    railing look like a flat grey square, and a right match look wrong."""
    from PIL import ImageDraw
    bg = Image.new('RGBA', (cell, cell), (90, 90, 90, 255))
    d = ImageDraw.Draw(bg)
    for y in range(0, cell, 8):
        for x in range(0, cell, 8):
            if (x // 8 + y // 8) % 2:
                d.rectangle([x, y, x + 7, y + 7], fill=(170, 170, 170, 255))
    bg.alpha_composite(img.convert('RGBA').resize((cell, cell), Image.NEAREST))
    return bg.convert('RGB')


def cmd_review(args):
    from PIL import ImageDraw
    arch = _open_arch(args.work)
    rows = [r for r in read_matches(args.work) if r['tier'] == args.tier]
    if args.sample and len(rows) > args.sample:
        step = len(rows) / args.sample
        rows = [rows[int(i * step)] for i in range(args.sample)]
    cell = 128
    cols = 4
    sheet = Image.new('RGB', (cols * (cell * 2 + 8), ((len(rows) + cols - 1) // cols) * (cell + 18)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, r in enumerate(rows):
        x = (i % cols) * (cell * 2 + 8)
        y = (i // cols) * (cell + 18)
        with Image.open(os.path.join(args.dump, r['dump'] + '.png')) as d:
            sheet.paste(_on_checker(d, cell), (x, y))
        sheet.paste(_on_checker(_android_image(arch, r['android_hash']), cell), (x + cell, y))
        draw.text((x + 2, y + cell + 2), '%s m%s %s' % (r['distance'], r['margin'][:5], r['android'][-20:]),
                  fill=(230, 230, 230))
    out = os.path.join(args.work, 'review_%s.png' % args.tier)
    sheet.save(out)
    print('wrote %s (%d pairs)' % (out, len(rows)))


# --- bind ---

def _ini_keys(ini_path):
    keys = set()
    if os.path.exists(ini_path):
        section = ''
        for line in open(ini_path, encoding='utf-8', errors='replace'):
            s = line.strip()
            if s.startswith('['):
                section = s.lower()
            elif section == '[hashes]' and '=' in s and not s.startswith('#'):
                keys.add(s.split('=')[0].strip().lower())
    return keys


DEFAULT_INI_HEAD = """# PPSSPP texture replacements for GTA: Liberty City Stories (ULUS10041).
[options]
version = 1
hash = xxh64
ignoreAddress = true
ignoreMipmap = 1

[hashes]
"""


def _append_hashes(ini_path, lines, comment):
    if not os.path.exists(ini_path):
        with open(ini_path, 'w', encoding='utf-8') as f:
            f.write(DEFAULT_INI_HEAD)
    text = open(ini_path, encoding='utf-8').read()
    if '[hashes]' not in text.lower():
        text += '\n[hashes]\n'
    # Appended at the end of the file, which is inside [hashes] as long as it is the last section;
    # if it is not, insert before the section after it.
    lower = text.lower()
    start = lower.index('[hashes]')
    nxt = lower.find('\n[', start + 1)
    block = '\n# %s\n%s\n' % (comment, '\n'.join(lines))
    text = text + block if nxt < 0 else text[:nxt + 1] + block.lstrip('\n') + text[nxt + 1:]
    with open(ini_path, 'w', encoding='utf-8') as f:
        f.write(text)


def _opaque_mean(rgba):
    """Per-channel mean over the pixels that are drawn (alpha > 0), as float32 0-255."""
    a = rgba[..., 3] > 8
    px = rgba[..., :3][a] if a.any() else rgba[..., :3].reshape(-1, 3)
    return px.astype(np.float32).mean(axis=0)


def colour_matched(hd, dump):
    """The HD art scaled per channel to the dump's average colour; returns (image, gains)."""
    rgba = np.asarray(hd.convert('RGBA')).copy()
    want = _opaque_mean(np.asarray(dump.convert('RGBA')))
    have = _opaque_mean(rgba)
    gains = np.clip(want / np.maximum(have, 1.0), *GAIN_LIMITS)
    rgba[..., :3] = np.clip(rgba[..., :3].astype(np.float32) * gains, 0, 255).astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA'), gains


def cmd_bind(args):
    if not args.dump:
        sys.exit('bind needs --dump: each HD texture is matched to the colour of the dump it replaces')
    arch = _open_arch(args.work)
    ini = os.path.join(args.pack, 'textures.ini')
    existing = _ini_keys(ini)
    # 'sure' binds the sure tier; 'likely' binds both. The review tier is never bound from here.
    accept = {'sure': ('sure',), 'likely': ('sure', 'likely')}.get(args.tier, ())
    rows = [r for r in read_matches(args.work) if r['tier'] in accept]
    written = {}            # android key -> [(gains, rel)]
    lines = []
    skipped = 0
    for r in rows:
        if r['dump'].lower() in existing:
            skipped += 1
            continue
        key = r['android'].lower()
        with Image.open(os.path.join(args.dump, r['dump'] + '.png')) as dump:
            image, gains = colour_matched(_android_image(arch, r['android_hash']), dump)
        rel = None
        for g, existing_rel in written.get(key, []):
            if np.abs(g - gains).max() <= GAIN_SAME:
                rel = existing_rel
                break
        if rel is None:
            n = len(written.get(key, []))
            rel = '%s/%s%s.png' % (PACK_SUBDIR, key, '' if n == 0 else '~%d' % n)
            path = os.path.join(args.pack, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            image.save(path)
            written.setdefault(key, []).append((gains, rel))
        lines.append('%s = %s' % (r['dump'].lower(), rel))
    if lines:
        _append_hashes(ini, lines, 'Android HD art, bound by Tools/lcshd.py (tier %s)' % args.tier)
    files = sum(len(v) for v in written.values())
    print('bound %d hashes to %d HD textures in %d files (%d hashes already in the ini, left alone)'
          % (len(lines), len(written), files, skipped))


# --- dds ---

def cmd_dds(args):
    texconv = shutil.which('texconv') or shutil.which('texconv.exe')
    if not texconv:
        sys.exit('texconv not found (winget install Microsoft.DirectXTex)')
    root = os.path.join(args.pack, PACK_SUBDIR)
    pngs = []
    for d, _, files in os.walk(root):
        pngs += [os.path.join(d, f) for f in files if f.lower().endswith('.png')]
    converted = 0
    for path in pngs:
        with Image.open(path) as img:
            alpha = img.mode == 'RGBA' and np.asarray(img.getchannel('A')).min() < 250
        fmt = 'BC7_UNORM' if alpha else 'BC1_UNORM'
        r = subprocess.run([texconv, '-nologo', '-y', '-m', '1', '-f', fmt, '-o', os.path.dirname(path), path],
                           capture_output=True, text=True)
        dds = os.path.splitext(path)[0] + '.dds'
        if r.returncode != 0 or not os.path.exists(dds):
            print('texconv failed on %s: %s' % (path, (r.stdout + r.stderr).strip()[-200:]))
            continue
        os.remove(path)
        converted += 1
    ini = os.path.join(args.pack, 'textures.ini')
    if os.path.exists(ini):
        text = open(ini, encoding='utf-8').read()
        out = []
        for line in text.splitlines():
            if '=' in line and ('%s/' % PACK_SUBDIR) in line and line.rstrip().lower().endswith('.png'):
                target = line.split('=', 1)[1].strip()
                if os.path.exists(os.path.join(args.pack, os.path.splitext(target)[0] + '.dds')):
                    line = line.rstrip()[:-4] + '.dds'
            out.append(line)
        with open(ini, 'w', encoding='utf-8') as f:
            f.write('\n'.join(out) + '\n')
    print('converted %d of %d PNGs to DDS' % (converted, len(pngs)))


# --- merge ---

def _read_pack_hashes(ini_path):
    """The [hashes] entries of another pack, in order: [(key, value)], comments dropped."""
    out = []
    section = ''
    for line in open(ini_path, encoding='utf-8', errors='replace').read().splitlines():
        t = line.strip()
        if t.startswith('['):
            section = t.lower()
        elif section == '[hashes]' and '=' in t and not t.startswith('#'):
            k, v = t.split('=', 1)
            out.append((k.strip().lower(), v.strip()))
    return out


def cmd_merge(args):
    """Bring a hand-made pack (the HD UI/HUD pack) into this one: its files are copied in beside
    android/, its hashes go in as one marked block at the top of [hashes], and any Android binding
    of the same hash is dropped - the hand-made art wins. Re-running replaces the block, so an
    updated source pack is merged again the same way."""
    src = os.path.abspath(args.source)
    src_ini = os.path.join(src, 'textures.ini')
    if not os.path.exists(src_ini):
        sys.exit('no textures.ini in %s' % src)
    entries = _read_pack_hashes(src_ini)
    keys = {k for k, _ in entries}

    copied = 0
    missing = []
    for _k, rel in entries:
        if not rel:
            continue
        a = os.path.join(src, rel)
        b = os.path.join(args.pack, rel)
        if not os.path.exists(a):
            missing.append(rel)
            continue
        os.makedirs(os.path.dirname(b), exist_ok=True)
        if not os.path.exists(b) or os.path.getmtime(a) > os.path.getmtime(b) or os.path.getsize(a) != os.path.getsize(b):
            shutil.copy2(a, b)
            copied += 1

    ini = os.path.join(args.pack, 'textures.ini')
    if not os.path.exists(ini):
        with open(ini, 'w', encoding='utf-8') as f:
            f.write(DEFAULT_INI_HEAD)
    name = os.path.basename(src.rstrip('/\\'))
    begin, end = '# --- begin %s ---' % name, '# --- end %s ---' % name
    lines = open(ini, encoding='utf-8').read().splitlines()
    out = []
    inside = False
    dropped = 0
    section = ''
    for line in lines:
        t = line.strip()
        if t == begin:
            inside = True
            continue
        if t == end:
            inside = False
            continue
        if inside:
            continue
        if t.startswith('['):
            section = t.lower()
        if (section == '[hashes]' and '=' in t and not t.startswith('#')
                and t.split('=', 1)[0].strip().lower() in keys):
            dropped += 1        # an Android binding of a hash the hand-made pack covers
            continue
        out.append(line)
    block = [begin, '# Hand-made art from %s, merged by Tools/lcshd.py merge. Wins over the Android bindings.' % name]
    block += ['%s = %s' % (k, v) for k, v in entries]
    block.append(end)
    at = next(i for i, l in enumerate(out) if l.strip().lower() == '[hashes]') + 1
    out[at:at] = block
    with open(ini, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    print('merged %d hashes from %s (%d files copied, %d Android bindings of the same hashes dropped)'
          % (len(entries), name, copied, dropped))
    if missing:
        print('missing in the source pack: %s' % ', '.join(missing))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['index', 'match', 'review', 'bind', 'dds', 'merge'])
    ap.add_argument('--work', default=DEFAULT_WORK)
    ap.add_argument('--obb', default=DEFAULT_OBB)
    ap.add_argument('--patch', default=DEFAULT_PATCH)
    ap.add_argument('--dump', help="PPSSPP's TEXTURES/ULUS10041/new")
    ap.add_argument('--pack', help="PPSSPP's TEXTURES/ULUS10041")
    ap.add_argument('--tier', default='sure', choices=[t[0] for t in TIERS] + ['review'])
    ap.add_argument('--sample', type=int, default=48)
    ap.add_argument('--from', dest='source', default=os.path.join(WORKSPACE, 'texturereplacements'),
                    help='merge: the hand-made pack to bring in (default: the HD UI pack)')
    args = ap.parse_args()
    {'index': cmd_index, 'match': cmd_match, 'review': cmd_review, 'bind': cmd_bind, 'dds': cmd_dds,
     'merge': cmd_merge}[args.command](args)


if __name__ == '__main__':
    main()
