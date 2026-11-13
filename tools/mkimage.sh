#!/bin/bash
# Build xnu-flashed-slot_a.img for one XNU run without touching the phone: mtkloader with the run's settings, the payload
# (kernel + RAM disk) and both packed into your own boot_a (backup/boot_a.img). tools/xnu-cycle.sh calls this and then
# flashes and runs it; for mac/hakios-paso3.sh (or windows/hakios-paso2.cmd) this is the whole of the PC's side.
#
#   XNU_KERNEL   the kernel (default: the HakiOS tree's own build, else its prebuilt one)
#   XNU_RAMDISK  md0, the root (default: the tree's prebuilt/userland/hello: one program with raw system calls, which the
#                kernel's mockfs presents as /sbin/launchd -- "boot with nothing of the phone's", docs/porting-mt6897.md)
#   XNU_SECS     how long the kernel runs before it hands the log back (default 45)
#   XNU_ARGS     the kernel's own command line (default cpus=1); XNU_CORES follows its cpus=
#   XNU_WATCH    when the watcher core resets the board if XNU has not come back (default XNU_SECS + 240); 0: no watcher,
#                which cpus=8 needs (the watcher takes a core of its own)
#   XNU_BOOT     the core XNU starts on (default 0, where LK leaves the loader; 7: the fastest Cortex-A715, see
#                XNU_BOOT_CORE in mtkloader.c)
#   HAKIOS_ROOT  the HakiOS tree, branch mtk (xnuloader/identity.h, xnuloader/mkpayload.py; see mtkloader/build.sh)
# Extra arguments go to the loader's compiler (e.g. -DXNU_CHECKPOINT=17).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${HAKIOS_ROOT:-}" ]; then
	if [ -d "$ROOT/../../xnuloader" ]; then HAKIOS_ROOT="$(cd "$ROOT/../.." && pwd)"; else HAKIOS_ROOT="$(cd "$ROOT/.." && pwd)"; fi
fi
export HAKIOS_ROOT
K_BUILT="$HAKIOS_ROOT/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
if [ -z "${XNU_KERNEL:-}" ]; then
	if [ -f "$K_BUILT" ]; then XNU_KERNEL=$K_BUILT; else XNU_KERNEL=$HAKIOS_ROOT/prebuilt/mach.development.bcm2837; fi
fi
XNU_RAMDISK=${XNU_RAMDISK:-$HAKIOS_ROOT/prebuilt/userland/hello}
SECS=${XNU_SECS:-45}
XNU_ARGS=${XNU_ARGS:-cpus=1}
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
BOOT=${XNU_BOOT:-0}
for f in "$XNU_KERNEL" "$XNU_RAMDISK" "$HAKIOS_ROOT/xnuloader/mkpayload.py" "$ROOT/backup/boot_a.img"; do
	[ -f "$f" ] || { echo "mkimage: no $f" >&2; exit 1; }
done

# test settings: Android by default, XNU only when asked (tools/boot-xnu.sh), watchdog off and the watcher on, XNU comes back
# by itself -- so a kernel that hangs costs a power cycle and not a rescue through BROM
"$ROOT/mtkloader/build.sh" -DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH -DXNU_BOOT_CORE=$BOOT \
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"" "$@" >/dev/null
# XNU_GPU_FW: the owner's own mali_csffw.bin (fw-gpu/, never committed), for -DXNU_GPU=1 (bsd/dev/redwood_mtkgpu.c)
python3 "$HAKIOS_ROOT/xnuloader/mkpayload.py" -o "$ROOT/build-payload.bin" kernel="$XNU_KERNEL" ramdisk="$XNU_RAMDISK" \
	${XNU_GPU_FW:+gpufw="$XNU_GPU_FW"}
python3 "$ROOT/mtkloader/build_flashed.py"
if [ "$WATCH" = 0 ]; then W="no watcher"; else W="watcher at ${WATCH} s"; fi
echo "mkimage: kernel $XNU_KERNEL, md0 $XNU_RAMDISK; args \"$XNU_ARGS\", $CORES cores from cpu$BOOT, back after ${SECS} s, $W"
