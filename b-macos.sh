#!/bin/bash
# macOS build for the LCS fork, with no Homebrew needed (the VCS fork's b-macos.sh).
#
# Upstream's macOS build wants `brew install sdl3 sdl3_ttf` and a system cmake. This does without
# both: cmake and ninja go in .venv (from PyPI), SDL3 and SDL3_ttf are built from their release
# tags into .deps, and the app lands at build/PPSSPPSDL.app (build-debug/ with --debug).
# Re-running only rebuilds what changed. `python3 Tools/lcspackage.py --zip` packages a Release.
set -e
cd "$(dirname "$0")"
ROOT="$PWD"

SDL_TAG=release-3.4.16
SDL_TTF_TAG=release-3.2.2

# The oldest macOS the app runs on: Apple Silicon's first, and the floor CMakeLists gives the app
# itself. The dependencies need the same one, or they quietly require whichever macOS built them.
DEPLOYMENT_TARGET=11.0

BUILD_TYPE=Release
BUILD_DIR=build
if [ "$1" = "--debug" ]; then
	BUILD_TYPE=Debug
	BUILD_DIR=build-debug
fi

# Keep the tool and dependency folders out of `git status` without touching .gitignore.
EXCLUDE="$(git rev-parse --git-path info/exclude)"
for d in .venv/ .deps/; do
	grep -qxF "$d" "$EXCLUDE" 2>/dev/null || echo "$d" >> "$EXCLUDE"
done

if [ ! -x .venv/bin/cmake ] || [ ! -x .venv/bin/ninja ]; then
	python3 -m venv .venv
	.venv/bin/pip install --quiet --upgrade pip
	.venv/bin/pip install --quiet cmake ninja
fi
export PATH="$ROOT/.venv/bin:$PATH"

# Dependencies built for a different minimum macOS are rebuilt rather than trusted.
if [ -d .deps ] && [ "$(cat .deps/deployment-target 2>/dev/null)" != "$DEPLOYMENT_TARGET" ]; then
	rm -rf .deps
fi

# build_dep <package> <tag> <repo> [extra cmake args...]
build_dep() {
	local pkg=$1 tag=$2 repo=$3
	shift 3
	if [ -f ".deps/lib/cmake/$pkg/${pkg}Config.cmake" ]; then
		return
	fi
	local src=".deps/src/$pkg"
	rm -rf "$src"
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" \
		--recurse-submodules --shallow-submodules "$repo" "$src"
	cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
		-DCMAKE_INSTALL_PREFIX="$ROOT/.deps" -DCMAKE_PREFIX_PATH="$ROOT/.deps" "$@"
	cmake --build "$src/build"
	cmake --install "$src/build"
}

build_dep SDL3 "$SDL_TAG" https://github.com/libsdl-org/SDL.git \
	-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
build_dep SDL3_ttf "$SDL_TTF_TAG" https://github.com/libsdl-org/SDL_ttf.git \
	-DSDLTTF_VENDORED=ON -DSDLTTF_SAMPLES=OFF
echo "$DEPLOYMENT_TARGET" > .deps/deployment-target

# Nothing from Homebrew, even when it is installed: CMake would otherwise link its libpng and
# fontconfig by their /opt/homebrew paths, and the app would start on this Mac and on no other.
# libpng is in ext/; fontconfig only finds fallback fonts for glyphs PPSSPP's own UI lacks.
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_PREFIX_PATH="$ROOT/.deps" \
	-DUSE_SYSTEM_LIBPNG=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Fontconfig=ON
cmake --build "$BUILD_DIR"
echo "Built $BUILD_DIR/PPSSPPSDL.app"
