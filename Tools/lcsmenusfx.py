"""Extract the game's own front-end sounds, for this fork's menu to click with.

A port of the VCS fork's Tools/vcsmenusfx.py, and it needed nothing but the paths: LCS keeps the
same three sounds in the same bank under the same names, uncompressed and NAMED:

    SFX_FE_HIGHLIGHT   the blip as the selection moves
    SFX_FE_SELECT      a row being chosen
    SFX_FE_BACK        going back a page

A `SET*/SFX*_PSP.RAW` file is a plain concatenation of Sony VAG streams, each with its own 0x30
header carrying its sample rate and its name; the header's `dataSize` is exact, so the next stream
starts immediately after. Nothing is compressed beyond the ADPCM itself and nothing is encrypted.

    python Tools/lcsmenusfx.py --bank ../disc/PSP_GAME/USRDIR/AUDIO/SET0/SFX3_PSP.RAW

Writes 16-bit mono WAVs into assets/lcs/, which ships with the build and is read through g_VFS at
runtime. PPSSPP's sample loader wants a "simple" WAV (raw 8- or 16-bit PCM), which is what this
writes; it resamples from the file's own rate, so the game's 25000 Hz is kept rather than
converted.

`--list` prints every stream in a bank. The front-end set also holds an error blip for each
direction and three noise bursts.
"""

import argparse
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_DIR = os.path.join(REPO, "assets", "lcs")
DEFAULT_BANK = os.path.join(os.path.dirname(REPO), "disc", "PSP_GAME", "USRDIR", "AUDIO", "SET0", "SFX3_PSP.RAW")

# What to pull out, and what the C++ side asks for it by. Keep the right-hand side in step with
# LoadDefaultSample in UI/BackgroundAudio.cpp.
WANTED = {
    "SFX_FE_HIGHLIGHT": "sfx_fe_highlight.wav",
    "SFX_FE_SELECT": "sfx_fe_select.wav",
    "SFX_FE_BACK": "sfx_fe_back.wav",
}

HEADER_SIZE = 0x30

# Sony ADPCM, the same four predictors every PS1-era decoder uses, scaled by 64.
FILTER_F0 = (0.0, 60.0 / 64.0, 115.0 / 64.0, 98.0 / 64.0, 122.0 / 64.0)
FILTER_F1 = (0.0, 0.0, -52.0 / 64.0, -55.0 / 64.0, -60.0 / 64.0)


def streams(data):
    """Every VAG stream in a bank, as (offset, name, rate, payload)."""
    out = []
    offset = 0
    while offset + HEADER_SIZE <= len(data):
        if data[offset:offset + 4] != b"VAGp":
            break
        size = struct.unpack_from(">I", data, offset + 0x0C)[0]
        rate = struct.unpack_from(">I", data, offset + 0x10)[0]
        name = data[offset + 0x20:offset + 0x30].split(b"\0")[0].decode("latin-1")
        start = offset + HEADER_SIZE
        out.append((offset, name, rate, data[start:start + size]))
        offset = start + size
    return out


def decode(payload):
    """VAG ADPCM to 16-bit PCM."""
    samples = []
    hist1 = hist2 = 0.0
    for block in range(0, len(payload) - 15, 16):
        head = payload[block]
        shift = head & 0x0F
        predictor = min(head >> 4, 4)
        flags = payload[block + 1]
        if flags == 7:  # end marker; the rest of the block is padding
            break
        for i in range(28):
            byte = payload[block + 2 + i // 2]
            nibble = (byte & 0x0F) if (i % 2 == 0) else (byte >> 4)
            if nibble > 7:
                nibble -= 16
            value = float(nibble << (12 - shift))
            value += hist1 * FILTER_F0[predictor] + hist2 * FILTER_F1[predictor]
            hist2 = hist1
            hist1 = value
            samples.append(max(-32768, min(32767, int(round(value)))))
    return samples


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bank", default=DEFAULT_BANK, help="SFX*_PSP.RAW to read (default: the front-end set)")
    ap.add_argument("--list", action="store_true", help="print every stream in the bank and stop")
    ap.add_argument("--out", default=OUT_DIR, help="where to write the WAVs")
    args = ap.parse_args()

    if not os.path.exists(args.bank):
        sys.exit("no bank at %s - point --bank at a dump of the disc" % args.bank)

    found = streams(open(args.bank, "rb").read())
    if not found:
        sys.exit("%s does not start with a VAG stream" % args.bank)

    if args.list:
        for offset, name, rate, payload in found:
            print("0x%06x  %6d bytes  %5d Hz  %s" % (offset, len(payload), rate, name))
        return

    os.makedirs(args.out, exist_ok=True)
    written = 0
    for _, name, rate, payload in found:
        filename = WANTED.get(name)
        if not filename:
            continue
        pcm = decode(payload)
        path = os.path.join(args.out, filename)
        with wave.open(path, "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
        print("%-18s -> %s  (%d samples, %d Hz, %.0f ms)" % (
            name, filename, len(pcm), rate, 1000.0 * len(pcm) / rate))
        written += 1

    missing = set(WANTED) - {name for _, name, _, _ in found}
    if missing:
        print("not in this bank: %s" % ", ".join(sorted(missing)), file=sys.stderr)
    if not written:
        sys.exit("nothing extracted - is this the front-end set? try --list")


if __name__ == "__main__":
    main()
