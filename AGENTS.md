# AGENTS.md

Notes for AI coding agents (and people) working in this repository. Read it, and hakios-mt6768's AGENTS.md (the reference
this was carried over from), before changing anything.

## What this is

HakiOS for the **POCO X6 Pro 5G (duchamp)**: MediaTek Dimensity 8300 Ultra (MT6897), Mali-G615 MC6, UFS, 8 or 12 GB of RAM.
The device repository; the kernel, user space and Metal driver are the common repositories'. Do not carry Qualcomm
assumptions (UEFI, Adreno, QMI) over, and do not carry MT6768 ones either: eMMC/MSDC, PWRAP, the Mali-G52's SPM power-up and
the TD4160 are not this phone's.

## Rules (as in the reference)

1. Nothing of Apple's in the tree. 2. Nothing of the vendor's: no firmware, no partition image, no device tree off a phone
(`logs/`, `fw-*/`, `backup/` are ignored). 3. No personal data: no serial numbers (scripts read `HAKIOS_SERIAL`).
4. `kernel/` is APSL 2.0, the rest MIT; GPL sources are read for addresses and named, never copied.
5. New code uses `hakios_*` names; the loader keeps the reference's names where it was carried over unchanged.
6. Commit messages say what the phone did in the run that proved the change.
7. Every address goes in `mtkloader/platform.h` with where it came from: `[dtb]`, `[phone]` or `[ref]`. A `[phone]` value
   becomes a fact only after `tools/duchamp-probe.sh` has shown it.

## Working on the phone: safety

Flash only `boot_a`. Always pass the serial. Keep `backup/` (tools/backup-boot.sh) before the first flash. Runs short and one
at a time; no voltage or clock changes; the UFS path, when it comes, read only by construction.
