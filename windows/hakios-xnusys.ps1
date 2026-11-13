# hakios-xnusys.ps1 -- Windows: la partición xnusys del POCO X6 Pro (duchamp), donde va la imagen del sistema de hakiOS.
#
# xnusys se recorta del final de userdata (8 GB). Eso obliga a formatear userdata: BORRA TODO lo que tienes en el
# teléfono (fotos, apps, cuentas). Las otras 89 particiones no se mueven ni se tocan.
#
# Menú:
#   1  LEER (no escribe nada): copia la tabla de particiones del UFS (LUN 2) y las particiones de calibración del
#      teléfono (nvram, nvdata, nvcfg, persist, protect1/2, proinfo: el IMEI y la calibración de radio) a backup\, y
#      deja backup\gpt-duchamp.zip para mandármelo. Con eso preparo la tabla nueva.
#   2  ESCRIBIR la tabla nueva (la que te mando: lun2-start.new y lun2-end.new en backup\gpt\) y borrar userdata.
#      Pide escribir BORRAR. Comprueba antes que la tabla del teléfono sigue siendo la que leyó la opción 1, y después
#      que se escribió bien y que el bootloader ve xnusys; solo entonces borra userdata y metadata en fastboot.
#
# Uso: doble clic en windows\hakios-xnusys.cmd

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
$Fastboot = Find-Tool 'fastboot.exe'
$Gpt = Join-Path $Root 'backup\gpt'
$Cal = Join-Path $Root 'backup\calibracion'
$Tmp = '/data/local/tmp/hakios-gpt'
$Region = 64                                    # bloques de 4 KB que se leen al principio y al final del LUN 2

function Adb-Serial {
	foreach ($l in (& $Adb devices)) { if ($l -match '^(\S+)\s+device$') { return $Matches[1] } }
	return $null
}
function Fastboot-Serial {
	foreach ($l in (& $Fastboot devices 2>&1)) { if ("$l" -match '^(\S+)\s+fastboot') { return $Matches[1] } }
	return $null
}
# una orden como root en el teléfono; su salida, recortada (la orden no lleva comillas simples)
function Root($s, $cmd) { return ((& $Adb -s $s shell "su -c '$cmd'" 2>&1) | Out-String).Trim() }
function Need-Root($s) {
	if ((Root $s 'id') -notmatch 'uid=0') { Fail 'No hay root (Magisk), o no le diste permiso a la shell.' }
}
# la carpeta de trabajo en el teléfono: de la shell, para que adb push y pull puedan; root escribe dentro
function Prep-Tmp($s) {
	Root $s "rm -rf $Tmp" | Out-Null
	& $Adb -s $s shell "mkdir -p $Tmp" | Out-Null
}
function Local-Sha($path) { return (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLower() }
function Phone-Sha($s, $path) { return ((Root $s "sha256sum $path") -split '\s+')[0].ToLower() }

# el LUN 2 (el disco que tiene userdata), su bloque y su tamaño
function Find-Disk($s) {
	$ud = Root $s 'readlink -f /dev/block/by-name/userdata'
	if (-not ($ud -match '^/dev/block/(sd[a-z])(\d+)$')) { Fail "userdata está en '$ud': no es lo que espero (/dev/block/sdXN). No sigo." }
	$d = @{ name = $Matches[1]; part = $Matches[2]; dev = "/dev/block/$($Matches[1])" }
	$d.lbs = [int64](Root $s "cat /sys/block/$($d.name)/queue/logical_block_size")
	if ($d.lbs -ne 4096) { Fail "el disco $($d.dev) tiene bloques de $($d.lbs) bytes, no de 4096. No sigo." }
	$d.blocks = [int64][math]::Floor([int64](Root $s "cat /sys/block/$($d.name)/size") / 8)
	$d.ud_start = [int64][math]::Floor([int64](Root $s "cat /sys/block/$($d.name)/$($d.name)$($d.part)/start") / 8)
	$d.ud_blocks = [int64][math]::Floor([int64](Root $s "cat /sys/block/$($d.name)/$($d.name)$($d.part)/size") / 8)
	$d.ro = (Root $s "blockdev --getro $($d.dev)")
	if ($d.blocks -lt 1000000 -or $d.ud_blocks -lt 1000000) { Fail "tamaños raros: disco $($d.blocks) bloques, userdata $($d.ud_blocks)." }
	return $d
}

# n bloques del disco desde el bloque 'from' a un archivo del PC; devuelve su SHA-256 (ya comparado con el del teléfono)
function Read-Region($s, $d, $from, $n, $local) {
	$remote = "$Tmp/" + (Split-Path -Leaf $local)
	Root $s "dd if=$($d.dev) of=$remote bs=4096 skip=$from count=$n 2>/dev/null; chmod 644 $remote" | Out-Null
	& $Adb -s $s pull $remote $local | Out-Null
	if (-not (Test-Path $local) -or (Get-Item $local).Length -ne $n * 4096) { Fail "no se pudo traer $local." }
	$h = Local-Sha $local
	if ((Phone-Sha $s $remote) -ne $h) { Fail "$local llegó distinto de como está en el teléfono." }
	return $h
}

function Option-Read {
	$s = Adb-Serial
	if (-not $s) { Fail 'No veo el teléfono en Android (depuración USB).' }
	Need-Root $s
	$d = Find-Disk $s
	if ((Root $s 'ls /dev/block/by-name/xnusys') -notmatch 'No such') { Warn 'xnusys ya existe en este teléfono. No hace falta nada de esto.'; return }
	Say "disco $($d.dev): $($d.blocks) bloques de 4 KB ($([math]::Round($d.blocks * 4096 / 1GB, 1)) GB); userdata desde el bloque $($d.ud_start), $($d.ud_blocks) bloques"
	New-Item -ItemType Directory -Force -Path $Gpt, $Cal | Out-Null
	Prep-Tmp $s

	Say 'leyendo la tabla de particiones (principio y final del disco)...'
	$h1 = Read-Region $s $d 0 $Region (Join-Path $Gpt 'lun2-start.bin')
	$h2 = Read-Region $s $d ($d.blocks - $Region) $Region (Join-Path $Gpt 'lun2-end.bin')

	Say 'copiando las particiones de calibración (se quedan en tu PC: tienen tu IMEI, no me las mandes)...'
	foreach ($p in @('proinfo', 'nvram', 'nvcfg', 'nvdata', 'persist', 'protect1', 'protect2')) {
		if ((Root $s "ls /dev/block/by-name/$p") -match 'No such') { Warn "  $p no existe en este teléfono"; continue }
		$remote = "$Tmp/$p.img"
		Root $s "dd if=/dev/block/by-name/$p of=$remote bs=1048576 2>/dev/null; chmod 644 $remote" | Out-Null
		$local = Join-Path $Cal "$p.img"
		& $Adb -s $s pull $remote $local | Out-Null
		if (-not (Test-Path $local) -or (Local-Sha $local) -ne (Phone-Sha $s $remote)) { Fail "la copia de $p no salió bien." }
		Root $s "rm -f $remote" | Out-Null
		Write-Host "  $p  $([math]::Round((Get-Item $local).Length / 1MB, 1)) MB"
	}
	Root $s "rm -rf $Tmp" | Out-Null

	$info = @(
		"device=$((& $Adb -s $s shell getprop ro.product.device | Out-String).Trim())",
		"slot=$((& $Adb -s $s shell getprop ro.boot.slot_suffix | Out-String).Trim())",
		"disk=$($d.dev)", "block_size=$($d.lbs)", "blocks=$($d.blocks)", "read_only=$($d.ro)",
		"userdata_part=$($d.name)$($d.part)", "userdata_start=$($d.ud_start)", "userdata_blocks=$($d.ud_blocks)",
		"region_blocks=$Region", "lun2-start.bin=$h1", "lun2-end.bin=$h2",
		"read=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
	)
	[IO.File]::WriteAllText((Join-Path $Gpt 'info.txt'), ($info -join "`n") + "`n")
	$zip = Join-Path $Root 'backup\gpt-duchamp.zip'
	Remove-Item -Force $zip -ErrorAction SilentlyContinue
	Compress-Archive -Path (Join-Path $Gpt 'lun2-start.bin'), (Join-Path $Gpt 'lun2-end.bin'), (Join-Path $Gpt 'info.txt') -DestinationPath $zip
	Say "listo. Mándame en el hilo SOLO este archivo: $zip"
	Say "las particiones de calibración quedaron en $Cal (guárdalas también fuera del PC)."
	Start-Process explorer.exe "/select,`"$zip`""
}

function Option-Write {
	$pri = Join-Path $Gpt 'lun2-start.new'; $sec = Join-Path $Gpt 'lun2-end.new'; $plan = Join-Path $Gpt 'plan.txt'
	foreach ($f in @($pri, $sec, $plan, (Join-Path $Gpt 'lun2-start.bin'), (Join-Path $Gpt 'lun2-end.bin'), (Join-Path $Gpt 'info.txt'))) {
		if (-not (Test-Path $f)) { Fail "falta $f. Primero la opción 1, y luego pon en backup\gpt\ los archivos que te mandé." }
	}
	if (-not (Test-Path (Join-Path $Cal 'nvdata.img'))) { Fail "no están las copias de calibración en $Cal (opción 1)." }
	$want = @{}
	foreach ($l in (Get-Content $plan)) { if ($l -match '^(\w+)=(\S+)') { $want[$Matches[1]] = $Matches[2] } }
	$info = @{}
	foreach ($l in (Get-Content (Join-Path $Gpt 'info.txt'))) { if ($l -match '^([\w.-]+)=(\S+)') { $info[$Matches[1]] = $Matches[2] } }
	if ((Local-Sha $pri) -ne $want['start_new_sha256'] -or (Local-Sha $sec) -ne $want['end_new_sha256']) {
		Fail 'lun2-start.new o lun2-end.new no son los que preparé (su SHA-256 no coincide con plan.txt).'
	}

	$s = Adb-Serial
	if (-not $s) { Fail 'No veo el teléfono en Android (depuración USB).' }
	Need-Root $s
	$d = Find-Disk $s
	if ("$($d.blocks)" -ne $info['blocks'] -or "$($d.blocks)" -ne $want['blocks']) { Fail 'el disco no mide lo que medía en la opción 1. No escribo nada.' }
	$priBlocks = [int64][math]::Floor((Get-Item $pri).Length / 4096); $secBlocks = [int64][math]::Floor((Get-Item $sec).Length / 4096)
	$secAt = $d.blocks - $secBlocks
	if ("$priBlocks" -ne $want['start_new_blocks'] -or "$secAt" -ne $want['end_new_at']) { Fail 'los archivos nuevos no encajan con plan.txt. No escribo nada.' }

	Write-Host ''
	Get-Content $plan | Where-Object { $_ -match '^#' } | ForEach-Object { Write-Host ($_ -replace '^# ?', '  ') }
	Write-Host ''
	Warn 'Esto BORRA TODO lo que hay en el teléfono: fotos, apps, chats, cuentas. No hay vuelta atrás.'
	Warn 'Antes: respalda lo tuyo, y ten a mano las contraseñas de tu cuenta de Google y de tu cuenta Xiaomi'
	Warn '(después del borrado el teléfono las pide). Batería por encima de 50 %.'
	if ((Read-Host 'Para seguir escribe BORRAR') -cne 'BORRAR') { Say 'No se escribió nada.'; return }

	Prep-Tmp $s
	Say 'comprobando que la tabla del teléfono sigue siendo la que leí...'
	$c1 = Join-Path $Gpt 'check-start.bin'; $c2 = Join-Path $Gpt 'check-end.bin'
	$k1 = Read-Region $s $d 0 $Region $c1
	$k2 = Read-Region $s $d ($d.blocks - $Region) $Region $c2
	Remove-Item -Force $c1, $c2
	if ($k1 -ne $info['lun2-start.bin'] -or $k2 -ne $info['lun2-end.bin']) { Fail 'la tabla del teléfono cambió desde la opción 1. No escribo nada: repite la opción 1 y mándamela.' }

	Say 'subiendo la tabla nueva al teléfono...'
	foreach ($f in @($pri, $sec)) {
		$remote = "$Tmp/" + (Split-Path -Leaf $f)
		& $Adb -s $s push $f $remote | Out-Null
		if ((Phone-Sha $s $remote) -ne (Local-Sha $f)) { Fail "$f llegó mal al teléfono. No se escribió nada." }
	}
	if ($d.ro -ne '0') { Root $s "blockdev --setrw $($d.dev)" | Out-Null }
	Say 'escribiendo la copia de respaldo de la tabla (final del disco) y luego la principal (bloque 1)...'
	Root $s "dd if=$Tmp/lun2-end.new of=$($d.dev) bs=4096 seek=$secAt conv=notrunc 2>&1; sync" | Out-Null
	Root $s "dd if=$Tmp/lun2-start.new of=$($d.dev) bs=4096 seek=1 conv=notrunc 2>&1; sync; blockdev --flushbufs $($d.dev)" | Out-Null
	Root $s "dd if=$($d.dev) of=$Tmp/back-start bs=4096 skip=1 count=$priBlocks 2>/dev/null; dd if=$($d.dev) of=$Tmp/back-end bs=4096 skip=$secAt count=$secBlocks 2>/dev/null" | Out-Null
	$ok1 = (Phone-Sha $s "$Tmp/back-start") -eq (Local-Sha $pri)
	$ok2 = (Phone-Sha $s "$Tmp/back-end") -eq (Local-Sha $sec)
	if (-not ($ok1 -and $ok2)) {
		Fail "lo que quedó en el disco no es la tabla nueva (principal: $ok1, respaldo: $ok2). NO reinicies el teléfono y mándame esta ventana."
	}
	Root $s "rm -rf $Tmp" | Out-Null
	Say 'la tabla nueva está escrita y verificada. Ahora al bootloader, para que la lea.'

	& $Adb -s $s reboot bootloader | Out-Null
	$f = $null
	for ($i = 0; $i -lt 90 -and -not $f; $i++) { Start-Sleep 1; $f = Fastboot-Serial }
	if (-not $f) { Fail 'El teléfono no aparece en fastboot. Mándame esta ventana; no apagues el teléfono.' }
	$x = ((& $Fastboot -s $f getvar partition-size:xnusys 2>&1) | Out-String)
	$u = ((& $Fastboot -s $f getvar partition-size:userdata 2>&1) | Out-String)
	Write-Host $x.Trim(); Write-Host $u.Trim()
	$seen = 0
	if ($x -match 'partition-size:xnusys:\s*0x([0-9a-fA-F]+)') { $seen = [Convert]::ToInt64($Matches[1], 16) }
	if ($seen -ne [int64]$want['xnusys_bytes']) {
		Fail 'el bootloader no ve xnusys con el tamaño esperado. No borro nada: mándame esta ventana.'
	}
	Say 'el bootloader ve xnusys. Borrando metadata y userdata (Android las rehace al arrancar)...'
	& $Fastboot -s $f erase metadata
	& $Fastboot -s $f erase userdata
	& $Fastboot -s $f reboot | Out-Null
	Say 'listo. El primer arranque tarda más y termina en la configuración inicial de Android.'
	Say 'Después: activa la depuración USB, autoriza este PC e instala otra vez la app de Magisk (el root sigue en boot_a).'
}

Write-Host ''
Write-Host '  1  LEER la tabla de particiones y respaldar la calibración (no escribe nada)'
Write-Host '  2  ESCRIBIR la tabla nueva con xnusys y BORRAR userdata'
$choice = Read-Host 'Elige 1 o 2'
if ($choice -eq '1') { Option-Read }
elseif ($choice -eq '2') { Option-Write }
else { Warn 'Nada que hacer.' }
Read-Host 'Pulsa Enter para salir' | Out-Null
