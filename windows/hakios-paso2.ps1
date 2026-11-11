# hakios-paso2.ps1 -- Windows: el paso 2 del port (la cadena, sin XNU) en el POCO X6 Pro (duchamp).
#
# Qué prueba: que el loader de hakiOS arranca desde boot_a delante del kernel de Android y le devuelve el control, y que su
# registro llega a pstore. NO arranca iOS ni XNU: la imagen no lleva XNU.
#
# Menú:
#   1  flashear xnu-flashed-slot_a.img en boot_a (solo boot_a, nunca el slot b ni otra partición)
#   2  probar: pedir al loader "una vez XNU" (sin XNU, debe decir que no hay payload y volver a Android) y traer su registro
#   3  RESTAURAR: volver a flashear tu boot_a original (backup\boot_a.img)
#
# Si el teléfono no vuelve a Android: mantén power + volumen abajo hasta que salga fastboot y usa la opción 3.
# Uso: doble clic en windows\hakios-paso2.cmd

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
	for ($i = 0; $i -lt 120; $i++) { $f = Fastboot-Serial; if ($f) { return $f }; Start-Sleep 1 }
	Fail 'El teléfono no aparece en fastboot (¿driver de fastboot instalado en Windows?).'
}
function Check-Image($path, $what) {
	if (-not (Test-Path $path)) { Fail "Falta $what ($path)." }
	$len = (Get-Item $path).Length
	if ($len -gt 67108864) { Fail "$what mide $len bytes: más que la partición boot (64 MB)." }
	$hdr = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path), 0, 8)
	if ($hdr -ne 'ANDROID!') { Fail "$what no es una imagen de arranque de Android." }
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
	Say 'reiniciando. Si todo va bien verás un momento "Booting Android" y luego Android normal.'
}
function Wait-Android {
	for ($i = 0; $i -lt 120; $i++) { $s = Adb-Serial; if ($s) { return $s }; Start-Sleep 3 }
	return $null
}

Write-Host ''
Write-Host '  1  flashear xnu-flashed-slot_a.img en boot_a'
Write-Host '  2  probar el loader (pide "XNU una vez", sin XNU) y traer su registro'
Write-Host '  3  RESTAURAR el boot_a original (backup\boot_a.img)'
$choice = Read-Host 'Elige 1, 2 o 3'

if ($choice -eq '1') {
	Check-Image $Backup 'la copia original backup\boot_a.img'
	Flash-BootA $Image 'xnu-flashed-slot_a.img'
	$s = Wait-Android
	if ($s) { Say 'Android volvió a arrancar a través del loader. Ahora ejecuta este script otra vez y elige 2.' }
	else { Warn 'Android no volvió en 6 minutos. Ponlo en fastboot (power + volumen abajo) y elige 3.' }
} elseif ($choice -eq '2') {
	$s = Adb-Serial
	if (-not $s) { Fail 'No veo el teléfono en Android (depuración USB).' }
	if ((& $Adb -s $s shell 'su -c id') -notmatch 'uid=0') { Fail 'No hay root (Magisk).' }
	Say 'pidiendo al loader un arranque de XNU (no hay XNU en la imagen: debe decirlo y volver a Android)...'
	& $Adb -s $s shell "su -c 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'" | Out-Null
	& $Adb -s $s reboot | Out-Null
	Start-Sleep 15
	$s = Wait-Android
	if (-not $s) { Fail 'Android no volvió en 6 minutos. Ponlo en fastboot (power + volumen abajo) y elige 3.' }
	Start-Sleep 5
	$logs = Join-Path $Root 'logs'; New-Item -ItemType Directory -Force -Path $logs | Out-Null
	$out = Join-Path $logs ("paso2-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
	$text = (& $Adb -s $s shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'") -join "`n"
	[IO.File]::WriteAllText($out, $text)
	if ($text -match 'mtkloader') { Say "el loader dejó su registro: $out  -> envíamelo en el hilo" }
	else { Warn "no aparece el registro del loader en pstore (lo que hubiera está en $out). Envíamelo igualmente." }
	Start-Process notepad.exe $out
} elseif ($choice -eq '3') {
	Flash-BootA $Backup 'tu boot_a original (backup\boot_a.img)'
	Say 'restaurado. El teléfono arranca como antes.'
} else { Warn 'Nada que hacer.' }
Read-Host 'Pulsa Enter para salir' | Out-Null
