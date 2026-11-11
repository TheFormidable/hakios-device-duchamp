#!/bin/bash
# Build mtkloader.bin for the POCO X6 Pro (duchamp, MT6897). MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0).
#
#   HAKIOS_ROOT   the HakiOS tree (hakios-xnu-kernel), for xnuloader/identity.h; default: two levels up when this repository
#                 is synced into devices/duchamp, else the directory above this repository
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
if [ -z "${HAKIOS_ROOT:-}" ]; then
	if [ -d "$REPO/../../xnuloader" ]; then HAKIOS_ROOT="$(cd "$REPO/../.." && pwd)"; else HAKIOS_ROOT="$(cd "$REPO/.." && pwd)"; fi
fi
[ -f "$HAKIOS_ROOT/xnuloader/identity.h" ] || {
	echo "mtkloader: no $HAKIOS_ROOT/xnuloader/identity.h -- xnuloader/mkidentity.sh your.ipsw (or --placeholder to only build)" >&2
	exit 1
}

# ld.lld: on the PATH, else zig's in the HakiOS tree
if command -v ld.lld >/dev/null; then LLD=ld.lld; else LLD="$HAKIOS_ROOT/xnu-deps/host/zig/zig ld.lld"; fi
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

# font8x16.h, the loader's text font, is made here and never kept in git (.gitignore): rasterized on a Mac by
# tools/mkfont8x16.swift from a system font, which is why the bitmap is not in the repository. Without a Mac, FONT=blank
# makes an empty one: the loader then writes nothing readable on the screen, and its log still goes to pstore.
if [ ! -f "$HERE/font8x16.h" ]; then
	if [ "${FONT:-}" = blank ]; then
		echo 'static const unsigned char font8x16[95][16] = { { 0 } };' > "$HERE/font8x16.h"
	else
		swift "$REPO/tools/mkfont8x16.swift" > "$HERE/font8x16.h"
	fi
fi

clang $CFLAGS "$@" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS -I "$HAKIOS_ROOT/xnuloader" "$@" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
