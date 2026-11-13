#!/usr/bin/env python3
"""
Pack xnu-flashed-slot_a.img: mtkloader, then Android's kernel, then the XNU payload, in YOUR phone's own boot_a image
(backup/boot_a.img by default, saved by tools/backup-boot.sh; with Magisk, the patched one -- on duchamp Magisk patches
init_boot, which this does not touch). Booting it runs XNU once if Android asked for it (tools/boot-xnu.sh), then Android
comes back with XNU's log in /sys/fs/pstore/console-ramoops-0 (see mtkloader/mtkloader.c).

  build_flashed.py [android-boot.img]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

android = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'backup', 'boot_a.img')
payload = os.path.join(ROOT, 'build-payload.bin')
args = [sys.executable, os.path.join(ROOT, 'tools', 'chainboot.py'), android,
        os.path.join(HERE, 'mtkloader.bin'), os.path.join(ROOT, 'xnu-flashed-slot_a.img')]
subprocess.check_call(args + ([payload] if os.path.exists(payload) else []))
