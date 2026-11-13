# hakios-sistema.ps1 -- Windows: escribe system.apfs (la imagen del sistema que construiste con las herramientas del
# proyecto) en la partición xnusys del POCO X6 Pro (duchamp). Es tools/system-flash.sh del árbol común, para Windows.
#
# Android tiene que estar arrancado, con la depuración USB y root (Magisk). Solo se escribe xnusys. La imagen pasa
# primero por /data/local/tmp (por una tubería de adb se pierden datos, como avisa system-flash.sh) y luego dd la copia.
# Antes comprueba que xnusys es de verdad xnusys (nombre, tamaño, que no es userdata) y que la imagen empieza por un
# contenedor APFS y cabe. Después lee xnusys de vuelta y compara su SHA-256 con el del archivo de tu PC.
#
# Uso: doble clic en hakios-sistema.cmd (busca system.apfs en la carpeta de arriba, en userland\ y en Descargas, o te
# lo pregunta), o arrastra system.apfs encima de hakios-sistema.cmd.

param([string]$Image = '')

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot

function Say($m)  { Write-Host "hakios: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "hakios: $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "hakios: ERROR: $m" -ForegroundColor Red; Read-Host 'Pulsa Enter para salir' | Out-Null; exit 1 }

function Find-Tool($name) {
	foreach ($c in @((Join-Path $Root "platform-tools\$name"), "C:\platform-tools\$name",
			(Join-Path $env:USERPROFILE "platform-tools\$name"), (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\$name"))) {
		if (Test-Path $c) { return $c }
	}
	$cmd = Get-Command $name -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	Fail "No encuentro $name (ni en C:\platform-tools ni en el PATH)."
}
$Adb = Find-Tool 'adb.exe'
$Tmp = '/data/local/tmp/hakios-sys'

function Adb-Serial {
	foreach ($l in (& $Adb devices)) { if ($l -match '^(\S+)\s+device$') { return $Matches[1] } }
	return $null
}
# una orden como root en el teléfono; su salida, recortada (la orden no lleva comillas simples)
function Root($s, $cmd) { return ((& $Adb -s $s shell "su -c '$cmd'" 2>&1) | Out-String).Trim() }
function Need-Root($s) {
	if ((Root $s 'id') -notmatch 'uid=0') { Fail 'No hay root (Magisk), o no le diste permiso a la shell.' }
}
function Local-Sha($path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function First-Word($text) { return (("$text".Trim()) -split '\s+')[0].ToLower() }

# ---- la imagen, en el PC ----
if (-not $Image) {
	foreach ($c in @((Join-Path $Root 'system.apfs'), (Join-Path $Root 'userland\system.apfs'), (Join-Path $PSScriptRoot 'system.apfs'),
			(Join-Path $env:USERPROFILE 'Downloads\system.apfs'))) {
		if (Test-Path -LiteralPath $c -PathType Leaf) { $Image = $c; break }
	}
}
if (-not $Image) { $Image = (Read-Host 'Arrastra aquí system.apfs y pulsa Enter').Trim().Trim('"') }
if (-not $Image -or -not (Test-Path -LiteralPath $Image -PathType Leaf)) { Fail "no encuentro la imagen: '$Image'" }
$Image = (Resolve-Path -LiteralPath $Image).Path
$size = [int64](Get-Item -LiteralPath $Image).Length
$fs = [IO.File]::OpenRead($Image); $hdr = New-Object byte[] 48; $got = $fs.Read($hdr, 0, 48); $fs.Close()
if ($got -ne 48 -or [Text.Encoding]::ASCII.GetString($hdr, 32, 4) -ne 'NXSB') {
	Fail "$Image no empieza por un contenedor APFS (NXSB en el byte 32): no es la imagen del sistema."
}
$bs = [int64][BitConverter]::ToUInt32($hdr, 36); $nb = [int64][BitConverter]::ToUInt64($hdr, 40)
if ($size % 4096 -ne 0) { Fail "$Image mide $size bytes, que no es un múltiplo de 4096: no parece entera." }
Say "imagen: $Image"
Say "  $([math]::Round($size / 1GB, 2)) GB; su primer contenedor APFS: $nb bloques de $bs bytes ($([math]::Round($nb * $bs / 1GB, 2)) GB)"
if ($nb * $bs -gt $size) { Warn '  el contenedor dice ser más grande que el archivo: ¿se copió entero del Mac?' }

# ---- el teléfono ----
$s = Adb-Serial
if (-not $s) { Fail 'No veo el teléfono en Android (depuración USB, y este PC autorizado).' }
Need-Root $s
$node = Root $s 'readlink -f /dev/block/by-name/xnusys'
if (-not ($node -match '^/dev/block/(sd[a-z]\d+)$')) { Fail "no hay xnusys en este teléfono ('$node')." }
$p = $Matches[1]
if ($node -eq (Root $s 'readlink -f /dev/block/by-name/userdata')) { Fail 'xnusys y userdata son la misma partición. No escribo nada.' }
if ((Root $s "cat /sys/class/block/$p/uevent") -notmatch 'PARTNAME=xnusys') { Fail "$node no se llama xnusys en la tabla. No escribo nada." }
$psize = [int64](Root $s "blockdev --getsize64 $node")
Say "xnusys: $node, $([math]::Round($psize / 1GB, 2)) GB"
if ($psize -lt 1GB -or $psize -gt 16GB) { Fail "xnusys mide $psize bytes: no es lo que espero. No escribo nada." }
if ($size -gt $psize -or $nb * $bs -gt $psize) { Fail 'la imagen no cabe en xnusys.' }
$now = Root $s "dd if=$node bs=4096 count=1 2>/dev/null | od -A n -t x1 -j 32 -N 4"
if ($now -match '4e\s+58\s+53\s+42') {
	Warn 'xnusys ya tiene una imagen APFS. Si sigues, la nueva la reemplaza.'
	if ((Read-Host 'Escribe S para seguir') -ne 'S') { Say 'No se escribió nada.'; exit 0 }
}
$free = 0
foreach ($l in ((Root $s 'df -k /data') -split "`n")) { if ($l -match '^\S+\s+\d+\s+\d+\s+(\d+)\s') { $free = [int64]$Matches[1] * 1024 } }
if ($free -lt $size + 256MB) { Fail "en /data quedan $([math]::Round($free / 1GB, 1)) GB y la imagen necesita $([math]::Round($size / 1GB, 1)) GB de paso." }

Say 'calculando el SHA-256 de la imagen en el PC (un minuto)...'
$h = Local-Sha $Image
Root $s "rm -rf $Tmp" | Out-Null
& $Adb -s $s shell "mkdir -p $Tmp" | Out-Null
Say 'subiendo la imagen al teléfono (unos minutos)...'
& $Adb -s $s push $Image "$Tmp/system.apfs"
if ((First-Word (Root $s "sha256sum $Tmp/system.apfs")) -ne $h) { Root $s "rm -rf $Tmp" | Out-Null; Fail 'la imagen llegó mal al teléfono. No se escribió nada en xnusys.' }

if ((Root $s "blockdev --getro $node") -ne '0') { Root $s "blockdev --setrw $node" | Out-Null }
Say 'escribiendo la imagen en xnusys...'
Write-Host (Root $s "dd if=$Tmp/system.apfs of=$node bs=4194304 2>&1; sync; blockdev --flushbufs $node")
Root $s "rm -rf $Tmp" | Out-Null
Say 'leyendo xnusys de vuelta para compararla...'
$back = First-Word (Root $s "dd if=$node bs=4096 count=$([int64][math]::Floor($size / 4096)) 2>/dev/null | sha256sum")
if ($back -ne $h) { Fail "lo que quedó en xnusys no es la imagen (SHA-256 $back, la imagen $h). Mándame esta ventana." }
Say "listo: xnusys tiene la imagen, idéntica a la de tu PC (SHA-256 $($h.Substring(0, 16))...)."
Say 'Android no la usa ni la toca; la lee XNU cuando arranca con la raíz en xnusys.'
Read-Host 'Pulsa Enter para salir' | Out-Null
