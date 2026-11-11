#!/bin/bash
# The POCO X6 Pro's firmware, out of YOUR phone's Android, into fw-*/ next to this repository's mtkloader/ (or the directory
# given). git ignores them: they are the vendor's, and nothing of the vendor's is in the repository (LEGAL.md).
#
#   fw-touch/     the touch controller's (FocalTech FT3683G or Goodix, whichever panel this phone has: xiaomi_touch)
#   fw-gpu/       the Mali-G615's CSF firmware (mali_csffw.bin) and GPUEB's, for the GPU step
#   fw-connsys/   CONNSYS's (gen4m Wi-Fi of the MT6897's CONNAC3, Bluetooth), for the Wi-Fi and Bluetooth steps
#   fw-scp/       the sensor hub's
#
# None of these is used yet: the first steps (docs/porting-mt6897.md) need no firmware. The names differ between Android
# builds, so they are looked for by pattern and listed; check the list. Needs root (Magisk's su). Read only.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE}"
TMP=/data/local/tmp/hakios-fw
declare -A WANT=(
	[touch]='-iname *focal* -o -iname *fts* -o -iname *goodix* -o -iname *gt9916* -o -iname *_ts_fw*'
	[gpu]='-iname mali_csffw* -o -iname *gpueb*'
	[connsys]='-iname WIFI_RAM_CODE* -o -iname *patch_mcu* -o -iname *ram_mcu* -o -iname *ram_wifi* -o -iname *ram_bt* -o -iname BT_RAM_CODE* -o -iname wifi.cfg -o -iname WMT_SOC.cfg'
	[scp]='-iname scp.img -o -iname *scp*.bin'
)
for k in "${!WANT[@]}"; do
	adb -s "$S" shell "su -c 'rm -rf $TMP/$k; mkdir -p $TMP/$k; \
		find /vendor/firmware /vendor/etc/firmware /odm/firmware /vendor/firmware_mnt -maxdepth 2 -type f \\( ${WANT[$k]} \\) \
		2>/dev/null | while read f; do cp \"\$f\" $TMP/$k/; done; chmod 644 $TMP/$k/* 2>/dev/null'" || true
	mkdir -p "$OUT/fw-$k"
	adb -s "$S" pull "$TMP/$k/." "$OUT/fw-$k/" >/dev/null 2>&1 || true
done
adb -s "$S" shell "su -c 'rm -rf $TMP'"
ls -l "$OUT"/fw-*
