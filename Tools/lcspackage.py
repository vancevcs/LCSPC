#!/usr/bin/env python3
"""Build the distributable folder for the LCS "PC version" fork (Windows, and macOS on a Mac).

    python Tools/lcspackage.py            # stage into dist/GTA.Liberty.City.Stories.PC (.Mac on a Mac)
    python Tools/lcspackage.py --zip      # stage and zip it

The VCS fork's Tools/vcspackage.py, Windows and Mac. What goes in is decided by one question: what
does a person who has never seen this repository need in order to play, and nothing else. On a Mac
the folder is the same with an app where the exe was (see the macOS section below).

THE MEMORY STICK IS THE WHOLE LAYOUT. Windows/main.cpp picks `exePath / "memstick"` unless a file
called `installed.txt` sits beside the exe, so a folder that ships its own `memstick/` and no
`installed.txt` is portable by construction: saves and settings stay inside it. That is also what
makes `memstick/PSP/LCS/ENGLISH.GXT` findable (the keyboard help text, Tools/lcsgxtkeys.py).

The exe is the Release build, renamed. It presents as the game because no `workshop.txt` sits
beside it (LCS::PresentAsGame): no PPSSPP logo, menu bar, ImGui debugger or log file, and the start
screen that finds the disc. The WebSocket debugger only listens with --debugger.

LEFT OUT, because each is something that isn't ours or isn't the player's:

  the ISO            copyrighted game data; everyone brings their own (the start screen finds it)
  memstick/SAVEDATA  saves; and the game loads the newest by itself, so a shipped one would put a
                     first-time player in somebody else's game
  memstick/PSP/PPSSPP_STATE, PLUGINS, Cheats, GAME, NAND   development scratch, or nothing to do with
                     this game
  SYSTEM/CACHE, DUMP, imgui.ini, imdebugger.ini   caches, the dump and log folder, the debugger's
                     window layout
  memstick/PSP/TEXTURES/*/new  where texture dumping writes (it stays on in every build: lcs.ini
                     DumpTextures; a player's dumps land there)
  assets/debugger    the WebSocket debugger's web UI
  ppsspp.ini's [Recent], [PlayTime], CurrentDirectory, the window position, the logging and debugger
                     switches; lcs.ini's GamePath, AutoSaveSlot and the two inverts (one person's)

Everything else ships, settings included: this is a build of one game tuned for weeks against these
values, and shipping upstream's defaults instead would be shipping a different program.
"""

import argparse
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILT_EXE = ROOT / "PPSSPPWindows64.exe"
EXE_NAME = "GTA Liberty City Stories.exe"
PACKAGE_NAME = "GTA.Liberty.City.Stories.PC"
DISC_ID = "ULUS10041"

INI_DROP_SECTIONS = {"Recent", "PlayTime"}
INI_DROP_KEYS = {"WindowX", "WindowY", "WindowWidth", "WindowHeight", "RunCount", "MacAddress"}
INI_FORCE = {
    "CurrentDirectory": "",          # this machine's folder
    "FileLogging": "False",          # the game build writes no log anyway
    "RemoteDebuggerOnStartup": "False",
    "CheckForNewVersion": "False",   # a fork: that feed offers a different program
    "ShowImDebugger": "False",       # refused at the draw in the game build, and not shipped open
    "DebugOverlay": "0",
    "ShowDeveloperMenu": "False",
    "AchievementsUserName": "",
    "iShowStatusFlags": "0",         # the FPS counter, a row of the menu's DISPLAY SETUP
}
LCS_INI_FORCE = {
    "GamePath": "",                  # a disc on this machine
    "AutoSaveSlot": "-1",            # this machine's saves
    # One person's preference, not the tuning: off is the code's default.
    "InvertMouseY": "False",
    "InvertAimY": "False",
}
SYSTEM_SKIP = {"imgui.ini", "imdebugger.ini"}
ASSET_SKIP = {"debugger"}

README = """Grand Theft Auto: Liberty City Stories - PC version
===================================================

Liberty City Stories, playing like a PC game: mouse look, mouse aiming,
WASD, a gamepad laid out like a PC game's, a menu of its own, HD textures,
and the game's own tutorial messages naming keys instead of PSP buttons.

It is a build of PPSSPP that only plays this one game.  No emulator to set
up, nothing to configure - everything is already set the way it was tuned.


Setting it up
-------------

1. Unzip this folder anywhere you like.  Nothing is installed.

2. Put your own copy of the game in this folder, next to
   "GTA Liberty City Stories.exe": the USA disc, ULUS10041, version 3.00,
   as an .iso or .cso.  It is not included and cannot be.

3. Run "GTA Liberty City Stories.exe".  It starts fullscreen, straight into
   the game.

The game checks the disc first.  If there is none in the folder, or it is
another release (Europe, or another version), it says what it found and
lets you choose one from anywhere on this computer.  The choice is
remembered.


Playing
-------

Escape opens the menu (Start on a pad).  OPTIONS -> CONTROLLER SETUP ->
KEY BINDINGS lists every control for the keyboard, an Xbox pad and a
PlayStation pad, side by side; any keyboard line can be given another key.

A few that are not obvious:

    Alt + WASD    walk instead of run
    G             side missions on and off - vigilante, taxi, paramedic
    Tab           answer the phone, swap for a weapon on the ground
    Q / E         look left and right in a car; with fire, a drive-by
    F (map)       place a marker - the radar then draws the route to it

The game saves by itself after every story mission, into the last slot,
marked (AUTO).  At a safe house the save icon opens the same menu's save
page.


Saves and settings
------------------

Everything the game writes lives in the "memstick" folder next to the exe.
Delete that folder and you are back to a fresh start; copy it and your
saves come with you.  Nothing is written anywhere else.

New textures the game draws are saved to memstick/PSP/TEXTURES/ULUS10041/
new - the HD pack is being completed from them.  Sending that folder in
helps.


If something is wrong
---------------------

Nothing happens when you run it, or Windows complains about a missing DLL
    Install the Microsoft Visual C++ Redistributable (x64) from Microsoft.

It takes a while to start
    The whole disc image is read into memory before the game boots, so the
    city streams in without stalling.  It needs about 2 GB of free memory.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this build - see LICENSE.TXT.  That licence entitles you to its complete
source code.

    Upstream PPSSPP:  https://github.com/hrydgard/ppsspp

The Pricedown typeface used by the menu is by Ray Larabie.  No game data of
any kind is included in this download.
"""


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def sanitise_ini(text, force, drop_sections=(), drop_keys=()):
    out = []
    section = None
    for line in text.splitlines():
        stripped = line.strip().lstrip("﻿")
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            if section not in drop_sections:
                out.append(line)
            continue
        if section in drop_sections:
            continue
        key = stripped.split("=", 1)[0].strip() if "=" in stripped else None
        if key in drop_keys:
            continue
        if key in force:
            out.append(f"{key} = {force[key]}")
            continue
        out.append(line)
    return "\n".join(out).rstrip() + "\n"


def copy_assets(dst):
    src = ROOT / "assets"
    if not src.is_dir():
        fail("assets/ is missing - run this from a checkout")
    dst.mkdir(parents=True, exist_ok=True)
    for entry in sorted(src.iterdir()):
        if entry.name in ASSET_SKIP or entry.suffix == ".bak":
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, dst / entry.name)


def stage_memstick(out_dir):
    psp = ROOT / "memstick" / "PSP"
    gxt = psp / "LCS" / "ENGLISH.GXT"
    if not gxt.is_file():
        fail("memstick/PSP/LCS/ENGLISH.GXT is missing - run Tools/lcsgxtkeys.py first")
    (out_dir / "memstick" / "PSP" / "LCS").mkdir(parents=True)
    shutil.copy2(gxt, out_dir / "memstick" / "PSP" / "LCS" / "ENGLISH.GXT")

    system = out_dir / "memstick" / "PSP" / "SYSTEM"
    system.mkdir(parents=True)
    for entry in sorted((psp / "SYSTEM").iterdir()):
        if entry.is_dir() or entry.name in SYSTEM_SKIP or ".bak" in entry.name:
            continue
        text = None
        if entry.name == "ppsspp.ini":
            text = sanitise_ini(entry.read_text(encoding="utf-8", errors="replace"), INI_FORCE,
                                INI_DROP_SECTIONS, INI_DROP_KEYS)
        elif entry.name == "lcs.ini":
            text = sanitise_ini(entry.read_text(encoding="utf-8", errors="replace"), LCS_INI_FORCE)
        if text is not None:
            (system / entry.name).write_text(text, encoding="utf-8")
        else:
            shutil.copy2(entry, system / entry.name)
    if not (system / "ppsspp.ini").is_file() or not (system / "lcs.ini").is_file():
        fail("memstick/PSP/SYSTEM needs ppsspp.ini and lcs.ini - they are what the package ships")

    src_tex = psp / "TEXTURES" / DISC_ID
    if not (src_tex / "textures.ini").is_file():
        fail("the texture pack (memstick/PSP/TEXTURES/ULUS10041) is missing")
    shutil.copytree(src_tex, out_dir / "memstick" / "PSP" / "TEXTURES" / DISC_ID,
                    ignore=shutil.ignore_patterns("new", "*.bak", "*.bak-*"))


def check_no_home_path(root):
    """Nothing that names this machine's home folder - and with it whoever built it - goes out.
    Checked in the bytes of every file, whatever route a path took in."""
    home = str(Path.home()).encode()
    leaks = [f for f in sorted(root.rglob("*")) if f.is_file() and home in f.read_bytes()]
    if leaks:
        shown = ", ".join(str(f.relative_to(root)) for f in leaks[:5])
        fail(f"{len(leaks)} file(s) still contain {home.decode()}: {shown}")


def build(out_dir, make_zip):
    if not BUILT_EXE.is_file():
        fail(f"{BUILT_EXE.name} not found - build PPSSPPWindows in Release x64 first")
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    shutil.copy2(BUILT_EXE, out_dir / EXE_NAME)
    copy_assets(out_dir / "assets")
    stage_memstick(out_dir)
    finish_package(out_dir, make_zip, README)


def finish_package(out_dir, make_zip, readme):
    shutil.copy2(ROOT / "LICENSE.TXT", out_dir / "LICENSE.TXT")
    (out_dir / "README.txt").write_text(readme, encoding="utf-8")
    for forbidden in ("installed.txt", "workshop.txt"):
        if (out_dir / forbidden).exists():
            fail(f"{forbidden} in the package would break it")
    check_no_home_path(out_dir)

    files = [f for f in out_dir.rglob("*") if f.is_file()]
    print(f"staged {len(files)} files, {sum(f.stat().st_size for f in files) / (1 << 20):.1f} MB -> {out_dir}")
    if make_zip:
        # Beside the folder, the whole name kept: with_suffix would take ".PC" for an extension.
        archive = out_dir.parent / (out_dir.name + ".zip")
        if archive.exists():
            archive.unlink()
        if sys.platform == "darwin":
            # ditto is what Finder's own Compress runs: it keeps an app bundle's symlinks, file modes
            # and code signature as they are, which zipfile does not promise.
            run("ditto", "-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--keepParent", out_dir, archive)
        else:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for f in sorted(files):
                    z.write(f, Path(out_dir.name) / f.relative_to(out_dir))
        print(f"zipped {archive.stat().st_size / (1 << 20):.1f} MB -> {archive}")


# --- macOS -------------------------------------------------------------------------------------
#
# The VCS fork's Mac package: the same folder, with an app where the exe was. Everything above about
# the memory stick still holds (NativeApp takes a `memstick` beside the app the way Windows/main.cpp
# takes one beside the exe), and assets/ is inside the bundle. The bundle b-macos.sh builds is
# PPSSPP's, renamed and re-dressed here rather than in CMakeLists, so the build files stay upstream's.

MAC_BUILT_APP = ROOT / "build" / "PPSSPPSDL.app"
MAC_PACKAGE_NAME = "GTA.Liberty.City.Stories.Mac"
MAC_APP_NAME = "GTA Liberty City Stories.app"
MAC_DISPLAY_NAME = "GTA Liberty City Stories"
MAC_BUNDLE_ID = "io.github.vancevcs.lcspc"
MAC_MIN_OS = "11.0"                  # b-macos.sh's DEPLOYMENT_TARGET
MAC_FRAMEWORKS_RPATH = "@executable_path/../Frameworks"
# The two libraries the app takes from outside macOS, both linked as @rpath/<name>. MoltenVK is
# already inside the bundle.
MAC_DEPS = ["libSDL3.0.dylib", "libSDL3_ttf.0.dylib"]
# How PPSSPP offers to open any .iso, .cso or .pbp on the Mac. A game has no business claiming the
# player's disc images, or changing what opens them.
MAC_PLIST_DROP = ("CFBundleDocumentTypes", "CFBundleURLTypes", "UTExportedTypeDeclarations")

MAC_README = """Grand Theft Auto: Liberty City Stories - PC version, for Mac
============================================================

Liberty City Stories, playing like a PC game: mouse look, mouse aiming,
WASD, a gamepad laid out like a PC game's, a menu of its own, HD textures,
and the game's own tutorial messages naming keys instead of PSP buttons.

It is a build of PPSSPP that only plays this one game.  No emulator to set
up, nothing to configure - everything is already set the way it was tuned.

It needs a Mac with Apple Silicon (M1 or newer) and macOS 11 or later.


Setting it up
-------------

1. Unzip this folder anywhere you like.  Nothing is installed.

2. Put your own copy of the game in this folder, next to
   "GTA Liberty City Stories": the USA disc, ULUS10041, version 3.00, as
   an .iso or .cso.  It is not included and cannot be.

3. Open "GTA Liberty City Stories".  It starts fullscreen, straight into
   the game.

The first time, macOS says it cannot check the app for malicious software.
That is because it is not signed with a paid Apple developer certificate,
not because anything is wrong with it.  Click Done, open System Settings ->
Privacy & Security, scroll down to the line about "GTA Liberty City
Stories", and click Open Anyway.  You only do this once.

If the folder is in Downloads, Documents or Desktop, macOS may also ask
whether the game can use files in that folder.  Allow it - your disc and
your saves are there.

The game checks the disc first.  If there is none in the folder, or it is
another release (Europe, or another version), it says what it found and
lets you choose one from anywhere on this Mac.  The choice is remembered.

Keep the folder together: the app and "memstick" are one thing.  Move the
folder and everything moves with it, saves included.


Playing
-------

Escape opens the menu (Start on a pad).  OPTIONS -> CONTROLLER SETUP ->
KEY BINDINGS lists every control for the keyboard, an Xbox pad and a
PlayStation pad, side by side; any keyboard line can be given another key.
Cmd+Q quits.

A few that are not obvious:

    Option + WASD  walk instead of run
    G              side missions on and off - vigilante, taxi, paramedic
    Tab            answer the phone, swap for a weapon on the ground
    Q / E          look left and right in a car; with fire, a drive-by
    F (map)        place a marker - the radar then draws the route to it

The game saves by itself after every story mission, into the last slot,
marked (AUTO).  At a safe house the save icon opens the same menu's save
page.


Saves and settings
------------------

Everything the game writes lives in the "memstick" folder next to the app.
Delete that folder and you are back to a fresh start; copy it and your
saves come with you.  Nothing is written anywhere else.

New textures the game draws are saved to memstick/PSP/TEXTURES/ULUS10041/
new - the HD pack is being completed from them.  Sending that folder in
helps.


If something is wrong
---------------------

macOS says the app is damaged, or there is no Open Anyway button
    Open Terminal, type the line below with a space at the end, drag this
    folder onto the Terminal window, and press Return:

        xattr -dr com.apple.quarantine

    Then open the app again.

It takes a while to start
    The whole disc image is read into memory before the game boots, so the
    city streams in without stalling.  It needs about 2 GB of free memory.


Licence
-------

PPSSPP is free software under the GNU GPL, version 2 or later, and so is
this build - see LICENSE.TXT.  That licence entitles you to its complete
source code.

    Upstream PPSSPP:  https://github.com/hrydgard/ppsspp

The app also carries SDL (zlib licence) and MoltenVK (Apache 2.0).  The
Pricedown typeface used by the menu is by Ray Larabie.  No game data of any
kind is included in this download.
"""


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


def output(*cmd):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True).stdout


def git_version():
    try:
        # The game's own release tags only: mac-* tags sit on the same commits.
        return output("git", "-C", ROOT, "describe", "--tags", "--match", "v*", "--always").strip().lstrip("v")
    except (OSError, subprocess.CalledProcessError):
        return "0"


def rpaths_of(binary):
    lines = output("otool", "-l", binary).splitlines()
    paths = []
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for following in lines[i + 1:i + 4]:
                following = following.strip()
                if following.startswith("path "):
                    paths.append(following[len("path "):].rsplit(" (offset", 1)[0])
    return paths


def ico_to_icns(ico, icns):
    """lcs.ico, the Windows build's icon, as the .icns a bundle wants. The .ico already holds PNGs
    from 16 to 256 pixels, so this repacks rather than resamples; 512 and 1024 are left out instead
    of being scaled up into blur."""
    data = ico.read_bytes()
    count = struct.unpack("<HHH", data[:6])[2]
    pngs = {}
    for i in range(count):
        width, _, _, _, _, _, size, offset = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        blob = data[offset:offset + size]
        if blob[:4] == b"\x89PNG":
            pngs[width or 256] = blob
    iconset_names = {
        "icon_16x16.png": 16, "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128, "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256,
    }
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "lcs.iconset"
        iconset.mkdir()
        for name, px in iconset_names.items():
            if px not in pngs:
                fail(f"{ico.name} has no {px}x{px} image for {name}")
            (iconset / name).write_bytes(pngs[px])
        run("iconutil", "-c", "icns", iconset, "-o", icns)


def build_mac(out_dir, make_zip):
    if not MAC_BUILT_APP.is_dir():
        fail(f"{MAC_BUILT_APP.relative_to(ROOT)} not found - run ./b-macos.sh first")
    for lib in MAC_DEPS:
        if not (ROOT / ".deps" / "lib" / lib).exists():
            fail(f".deps/lib/{lib} not found - run ./b-macos.sh first")
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    app = out_dir / MAC_APP_NAME
    shutil.copytree(MAC_BUILT_APP, app, symlinks=True)
    contents = app / "Contents"
    exe = contents / "MacOS" / "PPSSPPSDL"
    resources = contents / "Resources"
    frameworks = contents / "Frameworks"
    # The linker's debug map names every object and prebuilt library by its full path on this
    # machine (ffmpeg's, around 230 of them). -S takes only the debug entries; every symbol stays.
    run("xcrun", "strip", "-S", exe)

    # The same assets rule as Windows.
    for skip in ASSET_SKIP:
        shutil.rmtree(resources / "assets" / skip, ignore_errors=True)
    for bak in (resources / "assets").rglob("*.bak"):
        bak.unlink()

    # The game's icon and names in place of PPSSPP's.
    (resources / "ppsspp.icns").unlink(missing_ok=True)
    ico_to_icns(ROOT / "Windows" / "lcs.ico", resources / "lcs.icns")
    plist_path = contents / "Info.plist"
    with open(plist_path, "rb") as f:
        plist = plistlib.load(f)
    for key in MAC_PLIST_DROP:
        plist.pop(key, None)
    version = git_version()
    plist.update({
        "CFBundleName": MAC_DISPLAY_NAME,
        "CFBundleDisplayName": MAC_DISPLAY_NAME,
        "CFBundleIdentifier": MAC_BUNDLE_ID,
        "CFBundleIconFile": "lcs.icns",
        "CFBundleShortVersionString": version.split("-")[0],
        "CFBundleVersion": version,
        "LSMinimumSystemVersion": MAC_MIN_OS,
    })
    with open(plist_path, "wb") as f:
        plistlib.dump(plist, f)

    # SDL3 and SDL3_ttf into the bundle. Both are linked as @rpath/<name> already, so the only change
    # is where @rpath points: away from this machine's .deps, to the bundle's own Frameworks.
    frameworks.mkdir(exist_ok=True)
    for lib in MAC_DEPS:
        shutil.copy2((ROOT / ".deps" / "lib" / lib).resolve(), frameworks / lib)
    binaries = [exe] + sorted(frameworks.glob("*.dylib"))
    for binary in binaries:
        for rpath in rpaths_of(binary):
            if rpath.startswith("/"):
                run("install_name_tool", "-delete_rpath", rpath, binary)
    if MAC_FRAMEWORKS_RPATH not in rpaths_of(exe):
        run("install_name_tool", "-add_rpath", MAC_FRAMEWORKS_RPATH, exe)

    # Nothing in the bundle may link outside macOS and itself (this checkout, Homebrew): it would load
    # on this Mac and on no other, which is the one failure testing here cannot see.
    for binary in binaries:
        # otool names the file it is reading on lines ending in a colon (once more per architecture,
        # for MoltenVK); those are the bundle's own paths, not links.
        linked = [line.strip() for line in output("otool", "-L", binary).splitlines()
                  if not line.rstrip().endswith(":")]
        stray = [line for line in linked if not line.startswith(("/System/Library/", "/usr/lib/", "@"))]
        stray += [p for p in rpaths_of(binary) if p.startswith("/")]
        if stray:
            fail(f"{binary.name} still points at this machine: {stray}")

    # install_name_tool has invalidated the signatures, and Apple Silicon runs no unsigned binary.
    # Ad hoc (no identity), which is what "Open Anyway" in the README is about. Extended attributes
    # go first: codesign refuses a bundle that carries them.
    run("xattr", "-cr", app)
    for lib in sorted(frameworks.glob("*.dylib")):
        run("codesign", "--force", "--sign", "-", lib)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--strict", app)

    stage_memstick(out_dir)
    finish_package(out_dir, make_zip, MAC_README)


def main():
    mac = sys.platform == "darwin"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "dist" / (MAC_PACKAGE_NAME if mac else PACKAGE_NAME)),
                    help="where to stage the folder")
    ap.add_argument("--zip", action="store_true", help="zip the staged folder beside it")
    args = ap.parse_args()
    (build_mac if mac else build)(Path(args.out), args.zip)


if __name__ == "__main__":
    main()
