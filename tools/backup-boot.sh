#!/bin/bash
# Save YOUR phone's own boot_a (and vendor_boot_a, init_boot_a, dtbo_a, vbmeta_a) to backup/ before anything is flashed: it
# is how the phone is put back (fastboot flash boot_a backup/boot_a.img). Read only. git ignores backup/.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$HERE/backup"
for p in boot_a vendor_boot_a init_boot_a dtbo_a vbmeta_a; do
	adb -s "$S" shell "su -c 'cat /dev/block/by-name/$p'" > "$HERE/backup/$p.img"
	echo "$p: $(wc -c < "$HERE/backup/$p.img") bytes, sha256 $(sha256sum "$HERE/backup/$p.img" | cut -c1-16)"
done
head -c 8 "$HERE/backup/boot_a.img" | grep -q 'ANDROID!' || { echo "backup/boot_a.img is not a boot image"; exit 1; }
