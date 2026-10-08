# LCS HD textures from the Android build

`Tools/lcswad.py` reads the Android OBBs; `Tools/lcshd.py` binds their art to PPSSPP's texture
hashes by comparing pictures, then converts the result to DDS. The Android build (v2.2, War Drum,
2015) is Rockstar's own LCS art at four to eight times the PSP's size, so it is the texture source
(the guide's §8.3 decision).

## The OBB, as libGTALcs.so reads it

Read from the library's own code (symbols are exported: `WadArchive`, `WadFatEntry`,
`SerialiseEncryptDecryptBuffer`, `GetTXDTextureGetTexture`), not guessed. lcswad.py's header has
the layout; the short of it:

- **One XOR, by file position**: every byte is XORed with `AF 66`, `key[(pos + i) & 1]`.
  `ReadDataEncrypted` does exactly that in 0x2000-byte reads and nothing more.
- **"DAWL" container**: 1542 folder nodes at 0x800, then a 48-byte FAT entry per file (10,133 in
  main.17, 13 in patch.15), sorted by hash: hash, unused name offset, folder node, CRC, then 64-bit
  offset and size. Files are uncompressed.
- **Files are found by hash only**: CRC32 with an initial ~0 and no final XOR, of the lower-case
  path with `/` (`WadArchive::GetFatEntry` -> `Hash_CRC32CalcPKc` -> bsearch). The name strings
  were stripped from the shipped OBB, so names are recovered by hashing guesses (81 from strings in
  the library alone: `models/gta3.img`, `models/hud.txd`, the level `.img`/`.lvz` pairs...).
- **The game data is the PSP's**: `models/gta3.img` + `gta3.dir` (GTA III format, 4,971 entries),
  the four level areas as `.img` + `.lvz`, and TXDs still tagged `PSP` with 4-bit palettes. The HD
  art sits beside it, not in it.
- **The HD art**: `Textures_ETC/<txd>/<texture>.pvr`, where `<txd>/<texture>` is the PSP texture's
  name or what `master_texture_mapping_process.csv` (also in the OBB, 4,548 rows) remaps it to.
  9,178 files, 497 MB, each a legacy PVR (52-byte header ending `PVR!`): **ETC1** (format 0x36)
  for opaque art, **RGBA4444** (0x10) for art with alpha, square, 64-512 px, full mip chains. There
  is no DXT or PVRTC set in this OBB; the decoder for both formats is in lcswad.py.
- **Names**: the mapping CSV names 4,545 of them, the TXDs in `gta3.img` 5 more. The other ~4,600
  are world textures whose folders are named in the level files (`.lvz`), which are not read yet;
  they keep their folder node and hash (`folder1102/006857ce`). The matching does not need names.
- `patch.15` is the same format (13 files: string tables, two PVRs, a PNG); the game mounts it over
  main, and lcswad's `Archives` does the same.

## The pipeline

```
python Tools/lcshd.py index                       # decode and sign all 9,178 HD textures (~30 s)
python Tools/lcshd.py match  --dump <TEXTURES/ULUS10041/new>
python Tools/lcshd.py review --tier sure --dump ...   # contact sheets, alpha on a checkerboard
python Tools/lcshd.py bind   --pack <TEXTURES/ULUS10041> --dump .../new --tier sure
python Tools/lcshd.py dds    --pack <TEXTURES/ULUS10041>
python Tools/lcshd.py merge  --pack <TEXTURES/ULUS10041>        # the HD UI/HUD pack, see below
```

Work files go to `hd/` in the workspace root (the index, `matches.csv`, review sheets). The pack is
`memstick/PSP/TEXTURES/ULUS10041/`: `textures.ini` (the HUD pack's options - xxh64,
`ignoreAddress`, `ignoreMipmap` - so the hashes agree) and `android/`.

**Dumps come first.** PPSSPP names a texture by a hash of its data, and only a texture it has drawn
gets dumped: set `SaveNewTextures = True`, play, and the dump grows in `new/`. `match` tops up its
cache as it does. Coverage is the limit: the first dump (the car and park fixtures, two minutes)
has 867 textures.

### Matching

The VCS fork's `vcs_match` signature (a 16x16 RGB thumbnail, mean-subtracted per channel, unit
length; 1 - cosine), with what LCS needed on top:

- **No aspect penalty.** The Android art is square where a PSP texture can be 2:1 (18 of the 134
  bound); PPSSPP maps a replacement over the original's UV space whatever its shape, and the
  scene renders right.
- **A colour term** (`COLOUR_WEIGHT`): a dark and a white gradient have the same pattern and are
  not the same texture.
- **A second look** at the best 24 on a 32x32 grey grid, averaged in.
- **Margin, not distance, decides.** The margin is the gap to the best candidate that is a
  *different picture*: the Android set files the same art in several TXDs and at several sizes, so
  candidates within `SAME_PICTURE` of the best are skipped. On the first dump every wrong pair in
  the top tier was within 0.10 of a different picture; right ones were mostly far clear. Tiers:
  `sure` (distance <= 0.16, margin >= 0.12), `likely` (<= 0.22, >= 0.05), `review`.
- **Near-flat dumps are never bound** (`MIN_DETAIL`): a flat colour matches anything.
- **Look at alpha.** A railing dumped as a flat grey square with the pattern in its alpha is a
  right match that looks wrong if the sheet drops alpha; the review sheets draw on a checkerboard.

Measured on the first dump: 134 sure (all looked at: the wrong ones found sat at margins 0.08-0.10,
under the cut), 79 likely (roughly a quarter wrong - look before binding), 654 review.

### Brightness

**The PSP's textures are darker than the Android art**: over the bound set the dump's mean is a
median 0.62 of the HD art's (quartiles 0.51-0.75, a few near 1). That is half-intensity textures
that the GE doubles, plus the mobile port's own grading. Bound as they are, the HD textures came
out washed out (the park's dirt road went near-white). `bind` gives each HD texture the average
colour of the PSP texture it replaces, per channel (`colour_matched`, gains clamped to 0.25-2), so
the PSP's lighting is kept and only detail is added. One HD texture bound at different gains gets a
file per gain (`name~1.dds`).

### DDS

`dds` runs texconv on every bound PNG: **BC1** for opaque art, **BC7** for art with alpha, one level
(mips would hand back the PSP's resolution), and points the ini at the `.dds`. PPSSPP loads both
(`ReplacedTexture.cpp`: DXT1 fourcc and DX10 BC7). On the first set: 134 files, 3.1 MB of PNG ->
2.8 MB of DDS, and no replacement errors in the log. The gain is on the GPU side: no PNG decode on
load, and a quarter (BC1) or half (BC7) of the memory of RGBA8.

Checked in game: the car and park fixtures with the pack off, with PNGs and with DDS; the HD grass
and dirt draw, at the PSP's brightness after the colour match.

### The HD UI pack lives in the same folder

`merge` brings in the hand-made pack in `Dev/gtalcspc/texturereplacements` (text, HUD, radar,
weapons, buttons, map, front end: 137 hashes): its files are copied beside `android/`, its hashes go
in as one block marked `# --- begin/end texturereplacements ---` at the top of `[hashes]`, and an
Android binding of the same hash is dropped, so the hand-made art wins; `bind` then never adds one
back, since it skips hashes the ini already has. That pack stays the source: change it, re-run
`merge`, and the block is replaced. Its PNGs are not converted by `dds`, which only touches
`android/` - block compression smears text and thin UI lines.

Checked in game with both: the HD HUD (fist icon, digits, the help text's button glyph) over the
HD dirt and grass. The two packs share no hash so far. The pack's ini names `HUD/Chisel.png`, and
the file is `HUD/weapons/Chisel.png`, so that one icon does not load.

## Not done

- **Coverage**: everything not yet dumped. Play with `SaveNewTextures` on, then re-run `match`,
  `bind` and `dds`; `bind` skips hashes the ini already has.
- **The likely tier**: bind after looking at `review_likely.png`, or adjust tiers per area.
- **World texture names** from the `.lvz` level files, which would let names join where pictures
  are ambiguous (flat colours, near-identical tiles).
- **The 716 PNGs** in the OBB (`hud/`, `menu/`, `touch_fe/`, `fonts/`): the mobile UI, redone for
  touch. Not used: the HUD and front end have their own hand-made pack (below).
- Ped and vehicle textures that the PSP draws with palettes swapped per instance: each palette is a
  hash of its own, and each needs dumping.
