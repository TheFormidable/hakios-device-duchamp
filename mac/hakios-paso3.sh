#!/bin/bash
# hakios-paso3.sh -- macOS: el paso 3 del port en el POCO X6 Pro (duchamp): arrancar XNU una vez y traer su registro.
#
# La imagen (xnu-flashed-slot_a.img) es tu boot_a con el loader de hakiOS delante del kernel de Android y, detrás, el kernel
# XNU y un disco en RAM mínimo. Sigue arrancando Android como siempre; solo cuando se le pide (opción 2) arranca XNU UNA vez,
# lo deja correr unos segundos, vuelve, y Android arranca solo con el registro de XNU en pstore.
#
# Menú:
#   1  flashear xnu-flashed-slot_a.img en boot_a (solo boot_a, nunca el slot b ni otra partición)
#   2  arrancar XNU una vez y traer su registro a logs/
#   3  RESTAURAR tu boot_a original (backup/boot_a.img)
#   4  solo traer el registro (si tuviste que reiniciar el teléfono a mano)
#
# Pon en la misma carpeta: este script, xnu-flashed-slot_a.img y backup/boot_a.img (tu copia original).
# Uso, en Terminal:   bash ~/hakios-duchamp/hakios-paso3.sh
# Si el teléfono se queda colgado: mantén power ~15 s (se reinicia y arranca Android). Si no arranca Android: power +
# volumen abajo hasta fastboot, y opción 3.

HERE="$(cd "$(dirname "$0")" && pwd)"
IMG="$HERE/xnu-flashed-slot_a.img"
BACKUP="$HERE/backup/boot_a.img"
[ -f "$BACKUP" ] || [ ! -f "$HERE/boot_a.img" ] || BACKUP="$HERE/boot_a.img"

say()  { printf '\033[36mhakios: %s\033[0m\n' "$*"; }
warn() { printf '\033[33mhakios: %s\033[0m\n' "$*"; }
fail() { printf '\033[31mhakios: ERROR: %s\033[0m\n' "$*"; exit 1; }

# adb y fastboot: en el PATH, en ~/platform-tools o junto a este script; si no, los de Google para macOS
find_tools() {
	for d in "$HERE/platform-tools" "$HOME/platform-tools" "$HOME/Library/Android/sdk/platform-tools" /opt/homebrew/bin /usr/local/bin; do
		if [ -x "$d/adb" ] && [ -x "$d/fastboot" ]; then ADB="$d/adb"; FASTBOOT="$d/fastboot"; return; fi
	done
	if command -v adb >/dev/null && command -v fastboot >/dev/null; then ADB=$(command -v adb); FASTBOOT=$(command -v fastboot); return; fi
	say "no encuentro adb/fastboot: descargo platform-tools de Google para macOS..."
	curl -fL --retry 3 -o "$HERE/platform-tools.zip" https://dl.google.com/android/repository/platform-tools-latest-darwin.zip ||
		fail "no se pudo descargar platform-tools"
	(cd "$HERE" && unzip -q -o platform-tools.zip && rm -f platform-tools.zip) || fail "no se pudo descomprimir platform-tools"
	ADB="$HERE/platform-tools/adb"; FASTBOOT="$HERE/platform-tools/fastboot"
}
find_tools
say "adb: $ADB"

adb_serial() { "$ADB" devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1; exit}'; }
fastboot_serial() { "$FASTBOOT" devices 2>/dev/null | awk '$2=="fastboot" {print $1; exit}'; }

check_image() { # check_image <file> <qué>
	[ -f "$1" ] || fail "falta $2 ($1)"
	local len; len=$(stat -f %z "$1")
	[ "$len" -le 67108864 ] || fail "$2 mide $len bytes: más que la partición boot (64 MB)"
	[ "$(head -c 8 "$1")" = "ANDROID!" ] || fail "$2 no es una imagen de arranque de Android"
}

wait_fastboot() {
	local s f i
	s=$(adb_serial)
	if [ -n "$s" ]; then say "reiniciando en fastboot..."; "$ADB" -s "$s" reboot bootloader >/dev/null 2>&1
	else warn "no veo el teléfono en Android: ponlo en fastboot a mano (power + volumen abajo)"; fi
	say "esperando a fastboot..."
	for i in $(seq 1 90); do
		f=$(fastboot_serial)
		[ -n "$f" ] && { FB=$f; return 0; }
		[ $((i % 10)) -eq 0 ] && echo "  ...$i s"
		sleep 1
	done
	fail "el teléfono no aparece en fastboot. No se flasheó nada. Prueba otro cable o puerto USB."
}

flash_boot_a() { # flash_boot_a <file> <qué>
	check_image "$1" "$2"
	wait_fastboot
	"$FASTBOOT" -s "$FB" getvar current-slot 2>&1 | grep -q 'current-slot: *a' ||
		fail "el slot activo no es a ($("$FASTBOOT" -s "$FB" getvar current-slot 2>&1 | head -1)). No flasheo nada."
	say "flasheando $2 en boot_a..."
	"$FASTBOOT" -s "$FB" flash boot_a "$1" || fail "fastboot flash falló. El teléfono sigue en fastboot: usa la opción 3."
	"$FASTBOOT" -s "$FB" reboot >/dev/null 2>&1
}

wait_android() { # wait_android <segundos>
	local i s
	for i in $(seq 1 $(($1 / 3))); do
		s=$(adb_serial)
		[ -n "$s" ] && [ "$("$ADB" -s "$s" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && { SER=$s; return 0; }
		[ $((i % 20)) -eq 0 ] && echo "  ...$((i * 3)) s"
		sleep 3
	done
	return 1
}

fetch_log() {
	local out keep
	mkdir -p "$HERE/logs"
	out="$HERE/logs/paso3-$(date +%Y%m%d-%H%M%S).log"
	"$ADB" -s "$SER" shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'" > "$out" 2>/dev/null
	# lo que el kernel guardó para nosotros (zona pmsg), si es de esta ejecución y no de Android
	keep=$("$ADB" -s "$SER" shell "su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" 2>/dev/null)
	case "$keep" in *"kept for the host by the kernel"*) printf '\n%s\n' "$keep" >> "$out" ;; esac
	if grep -q "XNU log\|XNU returned\|ran until the watchdog" "$out"; then
		say "XNU dejó su registro: $out  -> mándamelo en el hilo"
	elif grep -q mtkloader "$out"; then
		warn "está el registro del loader pero no el de XNU: $out  -> mándamelo igual"
	else
		warn "no aparece el registro del loader en pstore: $out  -> mándamelo igual"
	fi
	open -e "$out" 2>/dev/null
}

need_root() {
	"$ADB" -s "$SER" shell 'su -c id' 2>/dev/null | grep -q 'uid=0' || fail "no hay root (Magisk) o no le diste permiso a la shell"
}

echo
echo '  1  flashear xnu-flashed-slot_a.img en boot_a'
echo '  2  arrancar XNU una vez y traer su registro'
echo '  3  RESTAURAR el boot_a original (backup/boot_a.img)'
echo '  4  solo traer el registro'
printf 'Elige 1, 2, 3 o 4: '
read -r choice

case "$choice" in
1)
	check_image "$BACKUP" "tu copia original backup/boot_a.img"
	flash_boot_a "$IMG" "xnu-flashed-slot_a.img"
	say "esperando a Android (arranca a través del loader)..."
	if wait_android 360; then say "Android volvió. Ahora ejecuta el script otra vez y elige 2."
	else warn "Android no volvió en 6 minutos. Ponlo en fastboot (power + volumen abajo) y elige 3."; fi
	;;
2)
	SER=$(adb_serial)
	[ -n "$SER" ] || fail "no veo el teléfono en Android (depuración USB activada y este Mac autorizado)"
	need_root
	say "pidiendo al loader un arranque de XNU..."
	"$ADB" -s "$SER" shell "su -c 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'" >/dev/null
	"$ADB" -s "$SER" reboot >/dev/null 2>&1
	say "XNU arranca ahora (la pantalla puede quedarse negra o con texto). Vuelve solo a Android en 1 a 6 minutos: no toques nada."
	sleep 20
	if wait_android 600; then
		sleep 5
		fetch_log
	else
		warn "Android no volvió en 10 minutos. Mantén power ~15 s para reiniciar; cuando arranque Android, ejecuta el script y elige 4."
	fi
	;;
3)
	flash_boot_a "$BACKUP" "tu boot_a original"
	say "restaurado. El teléfono arranca como antes."
	;;
4)
	SER=$(adb_serial)
	[ -n "$SER" ] || fail "no veo el teléfono en Android"
	need_root
	fetch_log
	;;
*) warn "nada que hacer" ;;
esac
