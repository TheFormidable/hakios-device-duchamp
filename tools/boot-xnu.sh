#!/bin/bash
# Reboot the duchamp into Darwin/XNU once: mtkloader starts Android unless it finds this request (kept across the reboot by
# ramoops' pmsg zone, which LK places and mtkloader finds in LK's tree); the boot after XNU is Android again.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
adb -s $S shell 'su -c "test -c /dev/pmsg0"' || { echo "no /dev/pmsg0 on this Android: the loader cannot be asked (see docs)"; exit 1; }
adb -s $S shell 'su -c "printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0"'
adb -s $S reboot
