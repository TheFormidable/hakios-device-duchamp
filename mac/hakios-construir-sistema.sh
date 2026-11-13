#!/bin/bash
# hakios-construir-sistema.sh -- macOS: construye las dos imágenes de hakiOS que van al POCO X6 Pro con userland/build.sh del
# árbol común, y las comprueba:
#   system.apfs   el sistema (unos 2.5 GB): va a la partición xnusys con windows\hakios-sistema.cmd
#   root.apfs     la pequeña (4 MB): nuestro init, etc/rc y los trabajos de launchd; va dentro de la imagen de arranque
#
# Qué hace, en orden (se puede volver a ejecutar: lo que ya está hecho se salta):
#   1. comprueba el árbol que dejó hakios-kernel.sh (~/hakios-build/hakios), las herramientas y el espacio libre
#   2. comprueba que el paso de los archivos de iOS 16.5 de la guía del árbol (docs/from-a-clone.md) está hecho.
#      Ese paso es tuyo y este script no lo hace: si falta, para aquí y te lo dice.
#   3. xnu-deps/setup.sh (ya lo hizo hakios-kernel.sh; se salta lo que está)
#   4. userland/build.sh con SHARE=1 (así root.apfs nunca lleva el nombre ni la contraseña de tu Wi-Fi)
#   5. comprueba las dos con el lector de APFS del propio árbol (tools/apfs/apfs.py, que verifica las sumas de cada
#      bloque que lee): que son contenedores APFS, que system.apfs tiene /sbin/launchd y cabe en xnusys (8 GB)
#   6. deja system.apfs en ~/Desktop/hakios-sistema/ y hakios-root-duchamp.zip (root.apfs y los SHA-256) en el
#      Escritorio, para que me lo mandes
#
# No toca el teléfono ni actualiza el árbol (sigue en la versión con la que compilaste el kernel).
# Uso, en Terminal:   bash ~/Downloads/hakios-construir-sistema.sh
# Si algo falla, el registro completo queda en ~/hakios-build/sistema-build.log: mándamelo.

W="$HOME/hakios-build"
LOG="$W/sistema-build.log"
TREE="$W/hakios"
XNUSYS_BYTES=8590950400                 # lo que mide xnusys en tu teléfono (blockdev --getsize64, 11 de noviembre)
DEST="$HOME/Desktop/hakios-sistema"
OUT="$HOME/Desktop/hakios-root-duchamp.zip"

mkdir -p "$W"
exec > >(tee -a "$LOG") 2>&1
say()  { printf '\033[36mhakios: %s\033[0m\n' "$*"; }
warn() { printf '\033[33mhakios: %s\033[0m\n' "$*"; }
fail() { printf '\033[31mhakios: ERROR: %s\033[0m\n' "$*"; echo "registro: $LOG"; exit 1; }

echo; say "=== $(date) ==="
[ "$(uname -s)" = Darwin ] || fail "este script es para macOS"

# 1. el árbol, las herramientas, el espacio
[ -d "$TREE/.git" ] && [ -f "$TREE/userland/build.sh" ] || fail "no está el árbol en $TREE: ejecuta antes hakios-kernel.sh, que lo clona y lo prepara"
cd "$TREE" || fail "no existe $TREE"
say "árbol: $(git log -1 --format='%h %ad' --date=short)"
for t in python3 git make cc zip shasum; do command -v $t >/dev/null || fail "no encuentro $t"; done
FREE_GB=$(df -g "$HOME" | awk 'NR==2 {print $4}')
[ "${FREE_GB:-0}" -ge 8 ] || fail "hacen falta unos 8 GB libres en el disco (hay ${FREE_GB} GB)"

# 2. los archivos de iOS 16.5 (el paso de la guía que es tuyo)
I=ios165
missing=
for m in .done-rootfs .done-cryptex .done-cryptex-app; do [ -f "$I/$m" ] || missing="$missing $m"; done
for d in rootfs cryptex cryptex-app '20F66__iPhone10,3_6'; do [ -d "$I/$d" ] || missing="$missing $d/"; done
if [ -n "$missing" ]; then
	fail "en $TREE/ios165 no están los archivos de iOS 16.5 (falta:$missing). Ese paso es tuyo: hazlo como dice la guía, $TREE/docs/from-a-clone.md, y vuelve a ejecutar este script."
fi
if [ "$I/select-system.py" -nt "$I/.done-rootfs" ]; then
	fail "la lista de archivos de iOS del árbol cambió desde que hiciste ese paso: repítelo como dice la guía ($TREE/docs/from-a-clone.md) y vuelve a ejecutar este script."
fi
for d in rootfs cryptex cryptex-app; do
	[ -L "$I/$d" ] && fail "ios165/$d es un enlace, no una carpeta: build.sh no construye así (lo explica en userland/build.sh)"
done
say "archivos de iOS: están ($(du -sh "$I/rootfs" | cut -f1) en ios165/rootfs)"

# 3. las dependencias
say "dependencias (xnu-deps/setup.sh)..."
xnu-deps/setup.sh || fail "xnu-deps/setup.sh falló"

# 4. las imágenes
say "construyendo system.apfs y root.apfs (unos 5 minutos; la primera vez algo más)..."
START=$(date +%s)
SHARE=1 ./userland/build.sh || fail "userland/build.sh falló. Busca la primera línea con 'error' en el registro."

# 5. comprobarlas
S=userland/system.apfs
R=userland/root.apfs
for f in $S $R; do [ -f "$f" ] || fail "no se generó $f"; done
for f in $S $R; do
	[ "$(dd if="$f" bs=1 skip=32 count=4 2>/dev/null)" = NXSB ] || fail "$f no empieza por un contenedor APFS"
done
SB=$(stat -f %z $S)
RB=$(stat -f %z $R)
[ $((SB % 4096)) -eq 0 ] || fail "system.apfs mide $SB bytes, que no es múltiplo de 4096"
[ "$SB" -le $XNUSYS_BYTES ] || fail "system.apfs mide $SB bytes y xnusys $XNUSYS_BYTES: no cabe"
[ "$RB" -le $((16 * 1024 * 1024)) ] || fail "root.apfs mide $RB bytes: con el kernel no cabría en la imagen de arranque"
python3 tools/apfs/apfs.py info $S > "$W/system-info.txt" || fail "el lector de APFS no puede leer system.apfs"
python3 tools/apfs/apfs.py stat $S /sbin/launchd > /dev/null || fail "system.apfs no tiene /sbin/launchd"
python3 tools/apfs/apfs.py ls $R / > "$W/root-ls.txt" || fail "el lector de APFS no puede leer root.apfs"
if python3 tools/apfs/apfs.py stat $R /fw/network > /dev/null 2>&1; then
	fail "root.apfs lleva fw/network (tu Wi-Fi): no me la mandes. Mándame este registro."
fi
[ "$(stat -f %m $S)" -ge "$START" ] || warn "system.apfs no cambió en esta vuelta (ya estaba hecha): la uso igual"
say "system.apfs: $((SB / 1048576)) MB, con /sbin/launchd; root.apfs: $((RB / 1024)) KB"

# 6. dejarlas a mano
mkdir -p "$DEST"
rm -f "$DEST/system.apfs"
cp -c $S "$DEST/system.apfs" 2>/dev/null || cp $S "$DEST/system.apfs" || fail "no se pudo copiar system.apfs a $DEST"
SSHA=$(shasum -a 256 "$DEST/system.apfs" | cut -d' ' -f1)
STAGE="$W/out-sistema"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp $R "$STAGE/root.apfs"
{
	echo "tree: $(git log -1 --format='%H %ad' --date=iso) (mtk)"
	echo "built: $(date) on macOS $(sw_vers -productVersion) $(uname -m), SHARE=1"
	echo "system.apfs: $SB bytes, sha256 $SSHA"
	echo "root.apfs: $RB bytes, sha256 $(shasum -a 256 "$STAGE/root.apfs" | cut -d' ' -f1)"
} > "$STAGE/build-info.txt"
cp "$STAGE/build-info.txt" "$DEST/"
rm -f "$OUT"
(cd "$STAGE" && zip -q -9 "$OUT" root.apfs build-info.txt) || fail "no se pudo crear $OUT"
say "listo."
say "1. Pasa $DEST/system.apfs al PC, a tu carpeta Descargas (son $((SB / 1048576)) MB; un pendrive sirve)."
say "   Allí windows\\hakios-sistema.cmd la encuentra sola y la escribe en xnusys."
say "2. Arrastra al hilo $OUT: con root.apfs armo la imagen de arranque."
open -R "$OUT" 2>/dev/null
exit 0
