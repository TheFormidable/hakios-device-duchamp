#!/system/bin/sh
# Runs ON THE PHONE as root (hakios-duchamp.ps1 pushes it and starts it with Magisk's su). Read only: it copies the boot
# partitions and reads what the probe wants into /data/local/tmp/hakios/out, which the PC then pulls and deletes.
# The serial number is cut out of everything it writes (LEGAL.md).
OUT=/data/local/tmp/hakios/out
rm -rf "$OUT"; mkdir -p "$OUT/backup" "$OUT/probe"
say() { echo "hakios: $*"; }

# ---- backup: both slots of the partitions the port will ever touch or need back ----
for p in boot vendor_boot init_boot dtbo vbmeta; do
	for s in _a _b; do
		dev=/dev/block/by-name/$p$s
		[ -e "$dev" ] || continue
		dd if="$dev" of="$OUT/backup/$p$s.img" bs=4194304 2>/dev/null && say "backup $p$s $(wc -c < "$OUT/backup/$p$s.img") bytes"
	done
done
( cd "$OUT/backup" && sha256sum *.img > SHA256SUMS.txt ) 2>/dev/null

# ---- probe ----
P=$OUT/probe
cat /sys/firmware/fdt > "$P/fdt.dtb"
cat /proc/iomem > "$P/iomem.txt"
tr ' ' '\n' < /proc/cmdline | grep -viE 'serial|imei|mac|uid|cid' > "$P/cmdline.txt"
{ ls -l /sys/fs/pstore/; ls -l /dev/pmsg0; for f in /sys/module/ramoops/parameters/*; do echo "$f=$(cat "$f")"; done; } \
	> "$P/pstore.txt" 2>&1
head -80 /proc/bootprof > "$P/bootprof.txt" 2>&1
{ ls -l /dev/block/by-name/; for p in boot_a boot_b vendor_boot_a init_boot_a userdata super; do
	[ -e /dev/block/by-name/$p ] && echo "$p $(blockdev --getsize64 /dev/block/by-name/$p)"; done; } > "$P/partitions.txt" 2>&1
{ grep -E 'processor|CPU part|CPU variant' /proc/cpuinfo; for c in /sys/devices/system/cpu/cpu[0-7]; do
	echo "$c $(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null)"; done; } > "$P/cpus.txt" 2>&1
dmesg | grep -iE 'focaltech|goodix|fts|gtx|touch|lcm|panel|videolfb|ufshcd|mali|gpueb|ramoops|pstore|Booting Linux|Machine model' \
	| grep -viE 'serial' | head -300 > "$P/dmesg.txt" 2>&1
{ getprop ro.product.device; getprop ro.product.model; getprop ro.build.version.incremental; getprop ro.build.version.release;
	echo "slot $(getprop ro.boot.slot_suffix)"; getprop ro.boot.flash.locked; getprop ro.boot.verifiedbootstate;
	wm size; wm density; } > "$P/android.txt" 2>&1
chmod -R a+rX /data/local/tmp/hakios
say "done"
