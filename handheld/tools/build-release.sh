#!/usr/bin/env bash
# Builds the two packages of the handheld port:
#
#   dist/Kerf-handheld-<version>-windows-x64.zip         kerf.exe + SDL2.dll + samples + notes
#   dist/Kerf-handheld-<version>-portmaster-aarch64.zip  Kerf.sh + kerf/ for PortMaster (RG35XX H ...)
#
# Needs Linux (or WSL), bash, python3 and Zig (https://ziglang.org/download/, or `pip install
# ziglang`). Zig is the C++ compiler for both targets, so no cross toolchain has to be installed.
# For Windows it also needs the SDL2 development package for MinGW-w64
# (SDL2-devel-<version>-mingw.tar.gz from https://github.com/libsdl-org/SDL/releases).
#
#   SDL2_WIN=/path/to/SDL2-2.30.11/x86_64-w64-mingw32 bash handheld/tools/build-release.sh
#
# Environment variables:
#   ZIG           the Zig command; default "zig" (use "python3 -m ziglang" for the pip package)
#   SDL2_WIN      the x86_64-w64-mingw32 folder of the SDL2 MinGW package (needed for Windows)
#   SDL2_INCLUDE  folder with SDL.h; default $SDL2_WIN/include/SDL2 (e.g. /usr/include/SDL2)
#   TARGETS       "windows portmaster" (default), or only one of them
#   OUT           where the zip files go; default handheld/dist
#   JOBS          parallel compiler runs; default 4
#
# The program for the handheld is linked against a small stand-in for libSDL2 that only exists
# at link time; it is not shipped. On the device the firmware's own SDL2 is used.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HANDHELD="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$HANDHELD/.." && pwd)"

ZIG="${ZIG:-zig}"
TARGETS="${TARGETS:-windows portmaster}"
OUT="${OUT:-$HANDHELD/dist}"
JOBS="${JOBS:-4}"
SDL2_WIN="${SDL2_WIN:-}"
SDL2_INCLUDE="${SDL2_INCLUDE:-${SDL2_WIN:+$SDL2_WIN/include/SDL2}}"

die() { echo "build-release: $*" >&2; exit 1; }

VERSION="$(sed -n 's/.*kVersion = "\([0-9][0-9.]*\)".*/\1/p' "$HANDHELD/src/main.cpp" | head -n1)"
[ -n "$VERSION" ] || die "cannot read the version from handheld/src/main.cpp"

command -v python3 >/dev/null || die "python3 is needed"
$ZIG version >/dev/null 2>&1 || die "Zig not found (ZIG='$ZIG'). Install it or run: pip install ziglang; ZIG='python3 -m ziglang' $0"
[ -n "$SDL2_INCLUDE" ] && [ -f "$SDL2_INCLUDE/SDL.h" ] || die "SDL.h not found: set SDL2_WIN (MinGW package) or SDL2_INCLUDE"

for t in $TARGETS; do
    case "$t" in
        windows)
            [ -f "$SDL2_WIN/lib/libSDL2.dll.a" ] && [ -f "$SDL2_WIN/bin/SDL2.dll" ] \
                || die "SDL2_WIN must be the x86_64-w64-mingw32 folder of the SDL2 MinGW package"
            ;;
        portmaster) ;;
        *) die "unknown target '$t' (use: windows, portmaster)" ;;
    esac
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# Every .cpp of the core and of the handheld program, except the headless runner.
mapfile -t SOURCES < <(cd "$ROOT" && ls core/src/*.cpp handheld/src/*.cpp | grep -v '/main_headless\.cpp$')

# ---------------------------------------------------------------------------------- compiling

compile_one() {   # compile_one <source relative to ROOT>   (uses ZIG TARGET FLAGS OBJDIR ROOT)
    local src="$1"
    local obj="$OBJDIR/$(echo "$src" | tr '/' '_').o"
    echo "  $src"
    ( cd "$ROOT" && $ZIG c++ -target "$TARGET" $FLAGS -c "$src" -o "$obj" )
}
export -f compile_one

build_objects() {  # build_objects <zig target> <objdir> <flags>
    export TARGET="$1" OBJDIR="$2" FLAGS="$3" ROOT ZIG
    mkdir -p "$OBJDIR"
    printf '%s\n' "${SOURCES[@]}" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}
}

object_list() {    # object_list <objdir>
    local f
    for f in "${SOURCES[@]}"; do echo "$1/$(echo "$f" | tr '/' '_').o"; done
}

COMMON_FLAGS="-std=c++20 -O2 -DNDEBUG -Wall -Wextra -Icore/src -Ihandheld/src -Ithird_party -I$SDL2_INCLUDE"

# ------------------------------------------------------------------------------------ packing

# mkzip <zip file> <manifest>: each manifest line is  <mode in octal> TAB <source> TAB <name in the zip>
mkzip() {
    python3 - "$1" "$2" <<'PY'
import sys, zipfile, os, time
zip_path, manifest = sys.argv[1], sys.argv[2]
epoch = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
stamp = time.gmtime(epoch)[:6]
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for line in open(manifest, encoding="utf-8").read().splitlines():
        if not line.strip():
            continue
        mode, src, name = line.split("\t")
        info = zipfile.ZipInfo(name, stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 3
        info.external_attr = (int(mode, 8) | (0o100000)) << 16
        with open(src, "rb") as f:
            z.writestr(info, f.read())
PY
}

# text <in> <out> <unix|windows>: UTF-8 text with the line endings of the target (windows: BOM + CRLF).
text() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys
src, dst, kind = sys.argv[1:4]
data = open(src, "rb").read().decode("utf-8-sig").replace("\r\n", "\n")
if kind == "windows":
    data = "﻿" + data.replace("\n", "\r\n")
open(dst, "wb").write(data.encode("utf-8"))
PY
}

licenses_into() {  # licenses_into <dir> <unix|windows>
    mkdir -p "$1"
    local f
    for f in "$ROOT"/third_party/licenses/*.txt; do text "$f" "$1/$(basename "$f")" "$2"; done
    [ -f "$ROOT/LICENSE" ] && text "$ROOT/LICENSE" "$1/Kerf-LICENSE.txt" "$2"
    return 0
}

# --------------------------------------------------------------------------------------- Windows

build_windows() {
    echo "== Windows x64"
    local obj="$WORK/win-obj" pkg="$WORK/win-pkg"
    build_objects x86_64-windows-gnu "$obj" "$COMMON_FLAGS"

    mkdir -p "$pkg/samples"
    # shellcheck disable=SC2046
    $ZIG c++ -target x86_64-windows-gnu $(object_list "$obj") -o "$pkg/kerf.exe" \
        "$SDL2_WIN/lib/libSDL2.dll.a" -lwinmm -lshell32 -Wl,--subsystem,windows -s
    cp "$SDL2_WIN/bin/SDL2.dll" "$pkg/SDL2.dll"
    cp "$HANDHELD/samples/demo_beat.wav" "$pkg/samples/"
    text "$HANDHELD/windows/LEIA-ME.txt" "$pkg/LEIA-ME.txt" windows
    licenses_into "$pkg/licenses" windows

    local top="Kerf-handheld-$VERSION-windows-x64"
    local manifest="$WORK/win.manifest"
    : > "$manifest"
    local f
    for f in kerf.exe SDL2.dll LEIA-ME.txt; do printf '644\t%s\t%s\n' "$pkg/$f" "$top/$f" >> "$manifest"; done
    printf '644\t%s\t%s\n' "$pkg/samples/demo_beat.wav" "$top/samples/demo_beat.wav" >> "$manifest"
    for f in "$pkg"/licenses/*; do printf '644\t%s\t%s\n' "$f" "$top/licenses/$(basename "$f")" >> "$manifest"; done

    mkzip "$OUT/$top.zip" "$manifest"
    echo "   -> $OUT/$top.zip"
}

# ------------------------------------------------------------------------------------ PortMaster

# The names of the SDL functions the program uses: read from the object files, so the stand-in
# library always matches the code.
sdl_symbols() {
    python3 - "$@" <<'PY'
import struct, sys
names = set()
for path in sys.argv[1:]:
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2, path + ": not a 64-bit ELF object"
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    for (name, typ, flags, addr, off, size, link, info, align, entsize) in sections:
        if typ != 2:                       # SHT_SYMTAB
            continue
        stroff = sections[link][4]
        for i in range(size // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", d, off + i * 24)
            if st_shndx == 0 and st_name:  # undefined
                end = d.index(b"\0", stroff + st_name)
                sym = d[stroff + st_name:end].decode()
                if sym.startswith("SDL_"):
                    names.add(sym)
print("\n".join(sorted(names)))
PY
}

build_portmaster() {
    echo "== PortMaster aarch64"
    local target="aarch64-linux-gnu.2.17"          # old glibc: runs on every firmware
    local obj="$WORK/arm-obj" pkg="$WORK/pm-pkg"
    build_objects "$target" "$obj" "$COMMON_FLAGS -mcpu=cortex_a53 -DSDL_DISABLE_IMMINTRIN_H"

    mkdir -p "$pkg/kerf/samples"
    local objs; objs="$(object_list "$obj")"
    # shellcheck disable=SC2086
    sdl_symbols $objs > "$WORK/sdl-symbols.txt"
    { echo "/* link-time stand-in for libSDL2, not shipped */"
      while read -r s; do echo "void $s(void) {}"; done < "$WORK/sdl-symbols.txt"; } > "$WORK/sdl_stub.c"
    $ZIG cc -target "$target" -shared -fPIC -Wl,-soname,libSDL2-2.0.so.0 "$WORK/sdl_stub.c" -o "$WORK/libSDL2-2.0.so.0"
    echo "   $(wc -l < "$WORK/sdl-symbols.txt") SDL functions used"

    # shellcheck disable=SC2086
    $ZIG c++ -target "$target" -mcpu=cortex_a53 $objs -o "$pkg/kerf/kerf.aarch64" \
        -L"$WORK" -l:libSDL2-2.0.so.0 -lpthread -s

    text "$HANDHELD/portmaster/Kerf.sh" "$pkg/Kerf.sh" unix
    local f
    for f in port.json gameinfo.xml README.md LEIA-ME.txt; do text "$HANDHELD/portmaster/$f" "$pkg/kerf/$f" unix; done
    cp "$HANDHELD/portmaster/screenshot.png" "$pkg/kerf/screenshot.png"
    cp "$HANDHELD/samples/demo_beat.wav" "$pkg/kerf/samples/"
    licenses_into "$pkg/kerf/licenses" unix

    local name="Kerf-handheld-$VERSION-portmaster-aarch64"
    local manifest="$WORK/pm.manifest"
    : > "$manifest"
    printf '755\t%s\t%s\n' "$pkg/Kerf.sh" "Kerf.sh" >> "$manifest"
    printf '755\t%s\t%s\n' "$pkg/kerf/kerf.aarch64" "kerf/kerf.aarch64" >> "$manifest"
    for f in port.json gameinfo.xml README.md LEIA-ME.txt screenshot.png; do
        printf '644\t%s\t%s\n' "$pkg/kerf/$f" "kerf/$f" >> "$manifest"
    done
    printf '644\t%s\t%s\n' "$pkg/kerf/samples/demo_beat.wav" "kerf/samples/demo_beat.wav" >> "$manifest"
    for f in "$pkg"/kerf/licenses/*; do printf '644\t%s\t%s\n' "$f" "kerf/licenses/$(basename "$f")" >> "$manifest"; done

    mkzip "$OUT/$name.zip" "$manifest"
    echo "   -> $OUT/$name.zip"
}

for t in $TARGETS; do
    case "$t" in
        windows) build_windows ;;
        portmaster) build_portmaster ;;
    esac
done

( cd "$OUT" && sha256sum Kerf-handheld-"$VERSION"-*.zip > SHA256SUMS.txt && cat SHA256SUMS.txt )
