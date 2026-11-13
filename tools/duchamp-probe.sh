#!/bin/bash
# What the shipped device tree cannot say about YOUR duchamp, read off its running Android (root, Magisk's su), into
# logs/probe/ -- everything platform.h marks [phone]. Read only: nothing on the phone is changed.
#
#   fdt.dtb          the tree LK booted Android with (/sys/firmware/fdt): LK's memory, ramoops, framebuffer, reserved memory
#   iomem.txt        /proc/iomem: where Android's kernel was put ("Kernel code"), what DRAM is free for XNU
#   summary.txt      the numbers platform.h wants, worked out of the two
#
# The serial number is never written down: androidboot.serialno and the like are cut out of the command line (LEGAL.md).
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${1:-$HERE/logs/probe}
mkdir -p "$OUT"
su() { adb -s "$S" shell "su -c '$1'"; }

[ "$(adb -s "$S" shell getprop ro.product.device | tr -d '\r')" = duchamp ] || echo "warning: ro.product.device is not duchamp" >&2
su 'cat /sys/firmware/fdt' > "$OUT/fdt.dtb"
su 'cat /proc/iomem' > "$OUT/iomem.txt"
su 'cat /proc/cmdline' | tr ' ' '\n' | grep -viE 'serial|imei|mac|uid|cid' > "$OUT/cmdline.txt" || true
su 'ls /sys/fs/pstore/; for f in /sys/module/ramoops/parameters/*; do echo "$f=$(cat $f)"; done' > "$OUT/pstore.txt" 2>&1 || true
su 'cat /proc/bootprof 2>/dev/null | head -80' > "$OUT/bootprof.txt" || true
su 'ls -l /dev/block/by-name/ | grep -E " (boot|vendor_boot|init_boot|dtbo|userdata|super)(_a|_b)? "; \
	for p in boot_a vendor_boot_a init_boot_a userdata; do echo "$p $(blockdev --getsize64 /dev/block/by-name/$p)"; done' \
	> "$OUT/partitions.txt" || true
su 'cat /proc/cpuinfo | grep -E "processor|CPU part|CPU variant"; for c in /sys/devices/system/cpu/cpu[0-7]; do \
	echo "$c $(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null)"; done' > "$OUT/cpus.txt" || true
su 'dmesg | grep -iE "focaltech|goodix|fts|gtx|touch|lcm|panel|videolfb|ufshcd|mali|gpueb|ramoops|pstore" | head -200' \
	> "$OUT/dmesg.txt" || true
adb -s "$S" shell 'wm size; wm density; getprop ro.boot.slot_suffix; getprop ro.build.version.incremental' > "$OUT/android.txt"

python3 -I - "$OUT" <<'PY'
import os, re, struct, sys
out = sys.argv[1]
blob = open(os.path.join(out, 'fdt.dtb'), 'rb').read()
assert blob[:4] == b'\xd0\x0d\xfe\xed', 'fdt.dtb is not a flattened device tree (was su allowed?)'
so, ss = struct.unpack('>II', blob[8:16]); slen = struct.unpack('>I', blob[32:36])[0]
strings = blob[ss:ss + slen]
nodes, path, at = {}, [], so
while True:
    t, = struct.unpack('>I', blob[at:at + 4]); at += 4
    if t == 1:
        e = blob.index(b'\0', at); path.append(blob[at:e].decode()); at = (e + 4) & ~3; nodes['/'.join(path)] = {}
    elif t == 2:
        path.pop()
    elif t == 3:
        ln, no = struct.unpack('>II', blob[at:at + 8]); at += 8
        nodes['/'.join(path)][strings[no:strings.index(b'\0', no)].decode()] = blob[at:at + ln]; at = (at + ln + 3) & ~3
    elif t == 9:
        break
lines = []
def say(s):
    lines.append(s); print(s)
ch = next((v for k, v in nodes.items() if k.endswith('/chosen') or k == '/chosen'), {})
v = ch.get('atag,videolfb')
if v and len(v) >= 20:
    base, found, fps, vram = struct.unpack_from('<QIII', v)
    name = v[20:].split(b'\0')[0].decode(errors='replace')
    say(f'framebuffer (chosen/atag,videolfb): base {base:#x} size {vram:#x} fps {fps} lcm_found {found} lcm "{name}"')
else:
    say('framebuffer: no chosen/atag,videolfb -- the loader will have no screen; look for "framebuffer" in reserved-memory')
for k, p in nodes.items():
    comp = p.get('compatible', b'')
    if b'ramoops' in comp:
        reg = p.get('reg', b'')
        vals = struct.unpack('>%dI' % (len(reg) // 4), reg)
        sz = {n: struct.unpack('>I', p[n])[0] for n in ('record-size', 'console-size', 'ftrace-size', 'pmsg-size') if n in p}
        say(f'ramoops ({k}): reg {[hex(x) for x in vals]} {", ".join(f"{a} {b:#x}" for a, b in sz.items())}')
    if '/reserved-memory/' in k and 'reg' in p:
        reg = p['reg']; vals = struct.unpack('>%dI' % (len(reg) // 4), reg)
        if len(vals) >= 4:
            say(f'reserved {k.split("/")[-1]}: {((vals[0] << 32) | vals[1]):#x} + {((vals[2] << 32) | vals[3]):#x}')
mem = nodes.get('/memory', {}).get('reg', b'')
if mem:
    vals = struct.unpack('>%dI' % (len(mem) // 4), mem)
    for i in range(0, len(vals) - 3, 4):
        say(f'memory: {((vals[i] << 32) | vals[i+1]):#x} + {((vals[i+2] << 32) | vals[i+3]):#x}')
for l in open(os.path.join(out, 'iomem.txt')):
    if re.search(r'Kernel code|System RAM', l):
        say('iomem: ' + l.strip())
open(os.path.join(out, 'summary.txt'), 'w').write('\n'.join(lines) + '\n')
PY
echo "probe: $OUT (summary.txt: compare with mtkloader/platform.h)"
