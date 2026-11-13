# hakios-paso3.ps1 -- Windows: el paso 3 del port en el POCO X6 Pro (duchamp): arrancar XNU una vez y traer su registro.
#
# La imagen (xnu-flashed-slot_a.img, en la carpeta de arriba de esta) es tu boot_a con el loader de hakiOS delante del kernel
# de Android y, detrás, el kernel XNU y un disco en RAM mínimo. Sigue arrancando Android como siempre; solo cuando se le pide
# (opción 2) arranca XNU UNA vez, lo deja correr 45 s, vuelve, y Android arranca solo con el registro de XNU en pstore.
#
# Menú:
#   1  flashear xnu-flashed-slot_a.img en boot_a (solo boot_a, nunca el slot b ni otra partición)
#   2  arrancar XNU una vez y traer su registro a logs\
#   3  RESTAURAR tu boot_a original (backup\boot_a.img)
#   4  solo traer el registro (si tuviste que reiniciar el teléfono a mano)
#
# Si el teléfono se queda colgado: mantén power ~15 s (se reinicia y arranca Android), luego opción 4. Si no arranca
# Android: power + volumen abajo hasta fastboot, y opción 3.
# Uso: doble clic en windows\hakios-paso3.cmd

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
$Image = Join-Path $Root 'xnu-flashed-slot_a.img'
$Backup = Join-Path $Root 'backup\boot_a.img'

function Adb-Serial {
	foreach ($l in (& $Adb devices)) { if ($l -match '^(\S+)\s+device$') { return $Matches[1] } }
	return $null
}
function Fastboot-Serial {
	foreach ($l in (& $Fastboot devices 2>&1)) { if ("$l" -match '^(\S+)\s+fastboot') { return $Matches[1] } }
	return $null
}
function Wait-Fastboot {
	$s = Adb-Serial
	if ($s) { Say 'reiniciando en fastboot...'; & $Adb -s $s reboot bootloader | Out-Null }
	else { Warn 'No veo el teléfono en Android: ponlo en fastboot a mano (power + volumen abajo).' }
	Say "esperando a que Windows vea el teléfono en fastboot..."
	for ($i = 0; $i -lt 90; $i++) {
		$f = Fastboot-Serial
		if ($f) { Say "en fastboot: listo"; return $f }
		if ($i -eq 20) {
			Warn 'Windows todavía no ve el teléfono en fastboot. Lo normal es que falte el driver de fastboot:'
			Warn '  Administrador de dispositivos -> si aparece "Android" con un triángulo amarillo, instala el'
			Warn '  "Google USB Driver" (o el driver de Xiaomi) y elige "Android Bootloader Interface".'
		}
		if ($i % 5 -eq 0) { Write-Host "  ...$i s" }
		Start-Sleep 1
	}
	Fail 'El teléfono no aparece en fastboot. NO se ha flasheado nada. Para salir de fastboot mantén power ~10 s.'
}
function Check-Image($path, $what) {
	if (-not (Test-Path $path)) { Fail "Falta $what ($path)." }
	$len = (Get-Item $path).Length
	if ($len -gt 67108864) { Fail "$what mide $len bytes: más que la partición boot (64 MB)." }
	$fs = [IO.File]::OpenRead($path); $b = New-Object byte[] 8; [void]$fs.Read($b, 0, 8); $fs.Close()
	if ([Text.Encoding]::ASCII.GetString($b) -ne 'ANDROID!') { Fail "$what no es una imagen de arranque de Android." }
	# fecha y huella de la imagen, para ver que es la que toca (el mensaje de cada corrida da su huella)
	$sha = (Get-FileHash -Algorithm SHA256 -Path $path).Hash.Substring(0, 8).ToLower()
	Say "$what`: $path, del $((Get-Item $path).LastWriteTime.ToString('yyyy-MM-dd HH:mm')), huella $sha"
}
function Flash-BootA($path, $what) {
	Check-Image $path $what
	$f = Wait-Fastboot
	$slot = ((& $Fastboot -s $f getvar current-slot 2>&1) | Out-String)
	if ($slot -notmatch 'current-slot:\s*a') { Fail "El slot activo no es a ($($slot.Trim())). No flasheo nada." }
	Say "flasheando $what en boot_a..."
	& $Fastboot -s $f flash boot_a $path
	if ($LASTEXITCODE -ne 0) { Fail 'fastboot flash falló. El teléfono sigue en fastboot: usa la opción 3 si hace falta.' }
	& $Fastboot -s $f reboot | Out-Null
}
# Android con el arranque terminado (sys.boot_completed = 1): antes de eso pstore puede no estar listo
function Wait-Android($seconds) {
	for ($i = 0; $i -lt $seconds / 3; $i++) {
		$s = Adb-Serial
		if ($s -and ((& $Adb -s $s shell getprop sys.boot_completed 2>$null | Out-String).Trim() -eq '1')) { return $s }
		if ($i -gt 0 -and $i % 20 -eq 0) { Write-Host "  ...$($i * 3) s" }
		Start-Sleep 3
	}
	return $null
}
function Need-Root($s) {
	if ((& $Adb -s $s shell 'su -c id') -notmatch 'uid=0') { Fail 'No hay root (Magisk), o no le diste permiso a la shell.' }
}
function Fetch-Log($s) {
	$logs = Join-Path $Root 'logs'; New-Item -ItemType Directory -Force -Path $logs | Out-Null
	$out = Join-Path $logs ("paso3-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
	$text = (& $Adb -s $s shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'") -join "`n"
	# lo que el kernel guardó para nosotros (zona pmsg), si es de esta ejecución y no de Android
	$keep = (& $Adb -s $s shell "su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" 2>$null) -join "`n"
	if ($keep -match 'kept for the host by the kernel') { $text = $text + "`n`n" + $keep }
	[IO.File]::WriteAllText($out, $text)
	if ($text -match 'XNU log|XNU returned|ran until the watchdog') { Say "XNU dejó su registro: $out  -> envíamelo en el hilo" }
	elseif ($text -match 'mtkloader') { Warn "está el registro del loader pero no el de XNU: $out  -> envíamelo igual" }
	else { Warn "no aparece el registro del loader en pstore: $out  -> envíamelo igual" }
	Start-Process notepad.exe $out
}

Write-Host ''
Write-Host '  1  flashear xnu-flashed-slot_a.img en boot_a'
Write-Host '  2  arrancar XNU una vez y traer su registro'
Write-Host '  3  RESTAURAR el boot_a original (backup\boot_a.img)'
Write-Host '  4  solo traer el registro'
$choice = Read-Host 'Elige 1, 2, 3 o 4'

if ($choice -eq '1') {
	Check-Image $Backup 'la copia original backup\boot_a.img'
	Flash-BootA $Image 'xnu-flashed-slot_a.img'
	Say 'esperando a Android (arranca a través del loader)...'
	$s = Wait-Android 360
	if ($s) { Say 'Android volvió. Ahora ejecuta este script otra vez y elige 2.' }
	else { Warn 'Android no volvió en 6 minutos. Ponlo en fastboot (power + volumen abajo) y elige 3.' }
} elseif ($choice -eq '2') {
	$s = Adb-Serial
	if (-not $s) { Fail 'No veo el teléfono en Android (depuración USB).' }
	Need-Root $s
	Say 'pidiendo al loader un arranque de XNU...'
	& $Adb -s $s shell "su -c 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'" | Out-Null
	& $Adb -s $s reboot | Out-Null
	Say 'XNU arranca ahora (la pantalla puede quedarse negra o con texto). Vuelve solo a Android en 1 a 6 minutos: no toques nada.'
	Start-Sleep 20
	$s = Wait-Android 600
	if (-not $s) { Fail 'Android no volvió en 10 minutos. Mantén power ~15 s; cuando arranque Android, ejecuta el script y elige 4.' }
	Start-Sleep 5
	Fetch-Log $s
} elseif ($choice -eq '3') {
	Flash-BootA $Backup 'tu boot_a original (backup\boot_a.img)'
	Say 'restaurado. El teléfono arranca como antes.'
} elseif ($choice -eq '4') {
	$s = Adb-Serial
	if (-not $s) { Fail 'No veo el teléfono en Android.' }
	Need-Root $s
	Fetch-Log $s
} else { Warn 'Nada que hacer.' }
Read-Host 'Pulsa Enter para salir' | Out-Null
