#!/bin/bash
# One XNU run on the duchamp (POCO X6 Pro): build mtkloader (extra args go to the compiler, e.g.
# -DXNU_CHECKPOINT=17), pack it with Android and the XNU payload, flash boot_a, let XNU run and
# Android come back, then print what XNU left in pstore. Slot b is never touched.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
SECS=${XNU_SECS:-45}
# the loader, the payload and the image: tools/mkimage.sh (XNU_KERNEL, XNU_RAMDISK, XNU_ARGS, XNU_WATCH are its)
[ -f "$ROOT/backup/boot_a.img" ] || { echo "no backup/boot_a.img: tools/backup-boot.sh first"; exit 1; }
XNU_SECS=$SECS "$ROOT/tools/mkimage.sh" "$@"
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there: a run would hang for hours)
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4
"$ROOT/tools/boot-xnu.sh"
echo "XNU is running; Android comes back by itself"
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
for i in $(seq 1 10); do adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
# what the kernel kept for the host (kern.hakios.host_keep, kern.redwood.host_keep in the earlier kernel: the pipeline
# recipes and the bring-ups' results, whole however long the run was),
# which mtkloader put into the pmsg zone: appended to the log, when it is this run's and not Android's own
adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
