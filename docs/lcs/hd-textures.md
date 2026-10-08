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
python Tools/lcstxd.py                            # the texture tool: PC packs + Android, auto-assign
python Tools/lcstxdweb.py                         # the browser-pane viewer, Android only
```

Work files go to `hd/` in the workspace root (the index, `matches.csv`, review sheets). The pack is
`memstick/PSP/TEXTURES/ULUS10041/`: `textures.ini` (the HUD pack's options - xxh64,
`ignoreAddress`, `ignoreMipmap` - so the hashes agree) and `android/`.

**Dumps come first.** PPSSPP names a texture by a hash of its data, and only a texture it has drawn
gets dumped, to `new/`. `match` tops up its cache as the dump grows. Coverage is the limit: the
first dump (the car and park fixtures, two minutes) has 867 textures; one boot into the save's
area added 146.

**Dumping is on at every boot.** lcs.ini's `DumpTextures` (default on; Debug -> LCS has the
checkbox, no menu page) makes `LCS::ApplyTextureDump` turn PPSSPP's `SaveNewTextures` on when the
EBOOT is recognised: after the game's own config has loaded and before the GPU comes up, which is
when the texture cache reads it. It never turns it off; with the key off, PPSSPP's own checkbox
decides. It also sets `IgnoreTextureFilenames`, which does nothing but skip a check PPSSPP runs only
while saving: file names in the ini with capitals get a six-second warning on every load of the
ini, and the hand-made pack is named that way (`HUD/Health.png`). Checked: with `SaveNewTextures =
False` in ppsspp.ini, a boot logged `LCS: texture dump on` and saved 146 new textures (867 ->
1,013). Video frames and PPGe's own textures are never dumped; a texture with a replacement isn't
either, so a bound one never comes back as new.

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

## The texture tool: lcstxd.py, ported from VCS

No published tool reads this OBB end to end: obbdec_lcs unpacks it, the GTAStories Texture Viewer
reads the PSP and PS2 TXDs, and Magic.TXD the RenderWare mobile TXDs of III, VC and SA. Nothing
joins the PVRs to PPSSPP's hashes. The VCS fork's TXD browser does that for PC art, so it is ported:
`Tools/lcsrw.py` (its `txd.py`, the RenderWare/IMG/image reader), `Tools/lcsmatch.py` (its
`vcs_match.py`) and `Tools/lcstxd.py` (its `txd_browser.py`), from gtavcspc 1193f89. Same window:
dictionaries, grid and detail panes, search across every source, per-texture "Find LCS matches",
Auto-assign (each dump beside up to 10 candidates spread across packs, pick one per row, Install),
Unmatched, Compare packs, resolution families. `python Tools/lcstxd.py` opens it on the defaults.

**Sources** (`DEFAULT_SOURCES`): the VCS fork's `gtavcspc/sources` (gta3upscaled, gta3ps2upscaled,
gtavcupscaled, gtasaupscaled1/2, vcde, vcde_gta3, vcde_sa, models gta vcs e3: 137,463 textures) and
the Android OBB, which lcsrw lists as dictionaries `android/<txd>` (a WAD hash stands in for the
chunk offset; levels decode one at a time) and lcsmatch as the pack `android`. 146,579 indexed.

**Caches**, one per source and keyed on its path, beside it: `.txd_index_<stamp>.npz` (signatures,
the VCS tool's own file name, so the two share the PC index) and `.txd_scan_<stamp>.npz` (the
listing; a cold scan of the PC folder took three minutes, the cache 0.8 s, the tool starts in 5 s).
The VCS index was built when the folder was `Desktop\sources`; `TxdIndex._adopt` took it over with
its paths moved instead of a rebuild. It lacked `models gta vcs e3` (14,014 textures), which
`top_up` indexed from the scan in about 40 s: whatever a source's scan lists that its index lacks is
decoded and added when a review starts. Reindex rescans and tops up; it no longer rebuilds all.

**Scoring** is the VCS signature with the alpha penalty and **no aspect penalty** (see Matching).
The VCS tool's "match VCS aspect" install option, which stretches a square replacement to the PSP's
shape, is not ported: its docstring says square art drawn unsqueezed comes out twice as wide, but
PPSSPP normalises UVs by the original's size (`gstate_c.curTextureWidth`, set from the GE whatever
the replacement is) and samples the replacement in 0-1, so it covers the same area. With "grow" that
option only made the file bigger. Dump signatures cache as `new/.lcs_match_index.npz`, families as
`new/.lcs_families.npz`.

**Pre-picks** (rows under 0.05 start chosen, as in VCS) follow two LCS rules. A dump below
`MIN_DETAIL` (luminance spread, lcshd's measure) is never pre-picked and says "flat: look": a near
zero distance means nothing on a flat colour. And of the candidates within 0.01 of the closest (the
same artwork) the largest is taken, and only if it is bigger than the PSP texture: the closest is
often a copy at the PSP's own size (SA's 64x64 shad_car beside two 256x256 ones at 0.000; the E3
build's 32x32 particles), which gains nothing.

**Install** keeps the PSP texture's brightness ("keep PSP brightness", default on;
`lcshd.colour_matched`, also a toggle in the compare window), converts to DDS (`lcshd.png_to_dds`),
and writes through `lcshd.set_manual` into the manual block, with a `# <hash> <- <source>` comment
above each entry. Paths: `pc/<pack>/<hash>.dds`, or for Android art the web viewer's
`android/<txd>/<texture>~<last 8>.dds`, so both tools read it back. A hand-made hash is refused;
a file a previous pick wrote is deleted once nothing names it.

Measured on the 1,013-texture dump: 965 rows with a candidate within 0.45; 277 pre-picked
(gta3ps2upscaled 158, gtasaupscaled1 27, gtasaupscaled2 24, models gta vcs e3 23, vcde 19,
gta3upscaled 13, android 10, vcde_sa 2, gtavcupscaled 1), installed into a copy of the pack in 27 s,
all DDS, nothing outside the manual block changed. LCS reuses GTA III's art, so the GTA III PS2
upscale is the best single source. In game (a copy of that install in the pack, the park fixture):
the PC art drew at the PSP's brightness with nothing washed out or broken. Not an A/B: Toni ran
from the fixture in both boots with no input sent, and the two captures ended in different places.

Tested without input: the windows were driven from a script (`Browser`, `auto_assign`,
`install_checked` with the confirmation answered) and captured with PrintWindow.

### The web viewer: lcstxdweb.py

The first viewer, kept for the browser pane: `python Tools/lcstxdweb.py` serves 127.0.0.1:8642.

- **Android HD**: all 9,178 textures by TXD (named ones first, then the unnamed world folders),
  searchable by name or OBB hash. One shows its size, format and mips, saves as a full-size PNG,
  lists the dumps bound to it, and ranks the dumps that look like it (lcshd's `rank` run the other
  way round). Thumbnails come from the PVR's own 128 px mip level, cached in `hd/thumbs/`.
- **PSP dumps**: everything in `new/`, filtered by what the ini does with it (not replaced,
  Android, manual, hand-made, no HD) or by matcher tier. One ranks the Android set against it, or
  finds HD textures by name, and shows the pick colour-matched beside the dump, squeezed to the
  dump's shape: that is how it draws (a 64x64 bound to a 32x64 covers the same UVs, so it draws
  1:2). The file keeps the Android size.
- **Bind** writes `android/<txd>/<texture>~<last 8 of the hash>.dds` through `lcshd.set_manual`,
  dropping an Android binding of the same hash. **No HD** writes an empty line, which PPSSPP reads
  as "no replacement" (and stops dumping it) and `bind` as taken, so a wrong automatic match stays
  gone. **Clear** takes the line out. A file the tools made is deleted when no line names it.
  `merge` and `bind` both leave the manual block alone; the hand-made block can't be changed here.
- PPSSPP rereads the ini on any graphics config change: Texture quality LOW and back to HIGH in the
  pause menu shows a bind in a running game (for either tool).

The server binds to 127.0.0.1 and refuses a POST without an `X-LCS` header, so another site open
in the browser can't write to the pack. Checked against a copy of the pack: bind, rebind (the old
file deleted), no HD on an automatic binding, clear, the hand-made refusal, and `merge` + `bind`
over the result; the rest of the ini came out byte-identical.

## Not done

- **Coverage**: everything not yet dumped. Every session dumps now; re-run `match`, then `bind`
  and `dds`, or Auto-assign in lcstxd. `bind` skips hashes the ini already has. After the first
  boot into the save: 1,012 scored, 157 sure (134 bound), 92 likely.
- **The likely tier**: bind after looking at `review_likely.png`, or adjust tiers per area.
- **World texture names** from the `.lvz` level files, which would let names join where pictures
  are ambiguous (flat colours, near-identical tiles).
- **The 716 PNGs** in the OBB (`hud/`, `menu/`, `touch_fe/`, `fonts/`): the mobile UI, redone for
  touch. Not used: the HUD and front end have their own hand-made pack (below).
- Ped and vehicle textures that the PSP draws with palettes swapped per instance: each palette is a
  hash of its own, and each needs dumping.
