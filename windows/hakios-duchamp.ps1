# hakios-duchamp.ps1 -- Windows (PowerShell 5.1 or 7): copia de seguridad y sondeo del POCO X6 Pro (duchamp) para el port de
# hakiOS. Es lo mismo que tools/backup-boot.sh + tools/duchamp-probe.sh, sin bash ni Python.
#
# Qué hace, en orden (SOLO LEE el teléfono: no flashea ni cambia nada):
#   1. busca adb (en el PATH o en .\platform-tools; si no está, ofrece descargar las platform-tools oficiales de Google)
#   2. elige el teléfono conectado (si hay varios, pregunta) y comprueba que hay root (Magisk)
#   3. copia al teléfono windows\phone-probe.sh y lo ejecuta con su: copia boot, vendor_boot, init_boot, dtbo y vbmeta de
#      los dos slots, y lee lo que el loader necesita saber (device tree de LK, /proc/iomem, ramoops, pantalla, táctil...)
#   4. trae todo al PC: backup\ (tu forma de restaurar el teléfono) y logs\probe\ ; borra la carpeta temporal del teléfono
#   5. comprueba las copias (SHA-256), escribe logs\probe\summary.txt y crea hakios-probe.zip para enviarlo en el hilo
#
# Uso: doble clic en windows\hakios-duchamp.cmd, o en PowerShell:  powershell -ExecutionPolicy Bypass -File windows\hakios-duchamp.ps1
# El número de serie del teléfono nunca se escribe en ningún archivo.

# 'Continue': adb writes its own messages to stderr, which 'Stop' would turn into errors; every step is checked by hand
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot           # la carpeta del repo (la de arriba de windows\)
$Remote = '/data/local/tmp/hakios'

function Say($m)  { Write-Host "hakios: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "hakios: $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "hakios: ERROR: $m" -ForegroundColor Red; Read-Host 'Pulsa Enter para salir' | Out-Null; exit 1 }

# ---------------------------------------------------------------- 1. adb
function Find-Adb {
	$local = Join-Path $Root 'platform-tools\adb.exe'
	if (Test-Path $local) { return $local }
	# donde se suelen descomprimir las platform-tools a mano
	foreach ($c in @('C:\platform-tools\adb.exe', (Join-Path $env:USERPROFILE 'platform-tools\adb.exe'),
			(Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'))) {
		if (Test-Path $c) { return $c }
	}
	$cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	Warn 'No encuentro adb.exe (ni en el PATH ni en .\platform-tools ni en C:\platform-tools).'
	$a = Read-Host '¿Descargo las platform-tools oficiales de Google (dl.google.com, ~13 MB) en esta carpeta? [S/n]'
	if ($a -match '^[nN]') { Fail 'Instala las Android platform-tools y vuelve a ejecutar.' }
	$zip = Join-Path $env:TEMP 'platform-tools-latest-windows.zip'
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
	Say 'Descargando platform-tools...'
	Invoke-WebRequest -UseBasicParsing -Uri 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $zip
	Expand-Archive -Path $zip -DestinationPath $Root -Force
	Remove-Item $zip -Force
	if (-not (Test-Path $local)) { Fail 'La descarga no dejó platform-tools\adb.exe.' }
	return $local
}

$Adb = Find-Adb
Say "adb: $Adb"
& $Adb start-server | Out-Null

# ---------------------------------------------------------------- 2. el teléfono
function Get-Phones {
	$lines = & $Adb devices
	$ok = @(); $bad = @()
	foreach ($l in $lines) {
		if ($l -match '^(\S+)\s+device$') { $ok += $Matches[1] }
		elseif ($l -match '^(\S+)\s+(unauthorized|offline|recovery|sideload)') { $bad += "$($Matches[1]) ($($Matches[2]))" }
	}
	return @{ ok = $ok; bad = $bad }
}

$ph = Get-Phones
while ($ph.ok.Count -eq 0) {
	if ($ph.bad.Count -gt 0) { Warn "Hay un teléfono pero no está listo: $($ph.bad -join ', '). Acepta 'Permitir depuración USB' en la pantalla del teléfono." }
	else { Warn 'No veo ningún teléfono. Conéctalo por USB con la depuración USB activada (Opciones de desarrollador).' }
	Read-Host 'Cuando esté, pulsa Enter (Ctrl+C para salir)' | Out-Null
	$ph = Get-Phones
}
if ($ph.ok.Count -eq 1) { $S = $ph.ok[0] }
else {
	for ($i = 0; $i -lt $ph.ok.Count; $i++) { Write-Host "  [$i] $($ph.ok[$i])" }
	$S = $ph.ok[[int](Read-Host 'Hay varios teléfonos: ¿cuál? (número)')]
}

function Phone([string]$cmd) { (& $Adb -s $S shell $cmd) -join "`n" }

$dev = (Phone 'getprop ro.product.device').Trim()
$model = (Phone 'getprop ro.product.model').Trim()
Say "teléfono: $model ($dev)"
if ($dev -ne 'duchamp') {
	Warn "Este no parece un POCO X6 Pro (ro.product.device = '$dev', se esperaba 'duchamp')."
	if ((Read-Host '¿Seguir igualmente? [s/N]') -notmatch '^[sS]') { exit 1 }
}

Say 'comprobando root (si Magisk pregunta en el teléfono, pulsa Permitir)...'
$id = Phone "su -c id"
if ($id -notmatch 'uid=0') { Fail "No hay root ('su -c id' respondió: $id). Instala Magisk (paso 2 de la guía) y dale permiso a 'Shell'." }

# ---------------------------------------------------------------- 3. el script del teléfono
$src = Join-Path $PSScriptRoot 'phone-probe.sh'
if (-not (Test-Path $src)) { Fail "Falta $src" }
# siempre con finales de línea de Unix: si git en Windows lo convirtió a CRLF, el sh del teléfono no lo entendería
$text = [IO.File]::ReadAllText($src) -replace "`r`n", "`n"
$tmpSh = Join-Path $env:TEMP 'hakios-phone-probe.sh'
[IO.File]::WriteAllBytes($tmpSh, [Text.Encoding]::ASCII.GetBytes($text))
& $Adb -s $S shell "rm -rf $Remote; mkdir -p $Remote" | Out-Null
& $Adb -s $S push $tmpSh "$Remote/phone-probe.sh" | Out-Null
Remove-Item $tmpSh -Force

Say 'leyendo el teléfono y copiando las particiones de arranque (1-2 minutos)...'
& $Adb -s $S shell "su -c 'sh $Remote/phone-probe.sh'"
if ($LASTEXITCODE -ne 0) { Warn "el script del teléfono terminó con código $LASTEXITCODE (sigo con lo que haya)" }

# ---------------------------------------------------------------- 4. al PC
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$pullTo = Join-Path $env:TEMP "hakios-pull-$stamp"
New-Item -ItemType Directory -Force -Path $pullTo | Out-Null
Say 'trayendo los archivos al PC...'
& $Adb -s $S pull "$Remote/out" $pullTo | Out-Null
$got = Join-Path $pullTo 'out'
if (-not (Test-Path $got)) { Fail 'No se pudo traer nada del teléfono (adb pull falló).' }
& $Adb -s $S shell "su -c 'rm -rf $Remote'" | Out-Null

$Backup = Join-Path $Root 'backup'
$Probe = Join-Path $Root 'logs\probe'
if (Test-Path $Backup) {
	$old = "$Backup-anterior-$stamp"
	Warn "ya había una carpeta backup\: la guardo como $(Split-Path -Leaf $old) (no se borra nada)"
	Move-Item $Backup $old
}
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'logs') | Out-Null
if (Test-Path $Probe) { Remove-Item -Recurse -Force $Probe }
Move-Item (Join-Path $got 'backup') $Backup
Move-Item (Join-Path $got 'probe') $Probe
Remove-Item -Recurse -Force $pullTo

# ---------------------------------------------------------------- 5. comprobar y resumir
$sums = Join-Path $Backup 'SHA256SUMS.txt'
$bad = 0
if (Test-Path $sums) {
	foreach ($l in Get-Content $sums) {
		if ($l -match '^([0-9a-f]{64})\s+(\S+)$') {
			$f = Join-Path $Backup $Matches[2]
			$h = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
			if ($h -ne $Matches[1]) { Warn "la copia de $($Matches[2]) no coincide con la del teléfono"; $bad++ }
		}
	}
} else { Warn 'no hay SHA256SUMS.txt: no puedo comprobar las copias'; $bad++ }
$boot = Join-Path $Backup 'boot_a.img'
if (-not (Test-Path $boot)) { Fail 'No hay backup\boot_a.img: no sigas sin esta copia.' }
$hdr = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($boot), 0, 8)
if ($hdr -ne 'ANDROID!') { Warn 'backup\boot_a.img no empieza por ANDROID!: algo salió mal en la copia'; $bad++ }
if ($bad -eq 0) { Say 'copias de seguridad comprobadas (SHA-256 iguales a las del teléfono)' }

# --- el device tree de LK: un lector mínimo de FDT, para summary.txt ---
function BE32([byte[]]$b, [int]$o) { return ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o+1] -shl 16) -bor ([uint32]$b[$o+2] -shl 8) -bor [uint32]$b[$o+3] }
function LE32([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt32($b, $o) }
function LE64([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt64($b, $o) }
function CStr([byte[]]$b, [int]$o) { $e = $o; while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }; return [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }
function Hex($v) { return ('0x{0:x}' -f [uint64]$v) }

$lines = New-Object System.Collections.Generic.List[string]
$fdtPath = Join-Path $Probe 'fdt.dtb'
try {
	$b = [IO.File]::ReadAllBytes($fdtPath)
	# 3490578157 = 0xd00dfeed (a hex literal would be a negative Int32 in PowerShell 5.1)
	if ((BE32 $b 0) -ne [uint32]3490578157) { throw 'fdt.dtb no es un device tree' }
	$offStruct = [int](BE32 $b 8); $offStr = [int](BE32 $b 12); $sizeStruct = [int](BE32 $b 36)
	$at = $offStruct; $end = $offStruct + $sizeStruct
	$path = New-Object System.Collections.Generic.List[string]
	$props = @{}                                   # props of the node being read
	$stack = New-Object System.Collections.Generic.List[hashtable]
	while ($at -lt $end) {
		$t = BE32 $b $at; $at += 4
		if ($t -eq 1) {
			$name = CStr $b $at; $at = ($at + $name.Length + 1 + 3) -band (-bnot 3)
			$path.Add($name); $props = @{}; $stack.Add($props)
		} elseif ($t -eq 3) {
			$len = [int](BE32 $b $at); $pn = CStr $b ($offStr + [int](BE32 $b ($at + 4))); $vo = $at + 8
			$props[$pn] = @($vo, $len); $at = ($vo + $len + 3) -band (-bnot 3)
		} elseif ($t -eq 2) {
			$p = '/' + (($path | Select-Object -Skip 1) -join '/')
			$cur = $stack[$stack.Count - 1]
			if ($p -eq '/chosen' -and $cur.ContainsKey('atag,videolfb')) {
				$o = $cur['atag,videolfb'][0]; $l = $cur['atag,videolfb'][1]
				$name = ''; if ($l -gt 20) { $name = CStr $b ($o + 20) }
				$lines.Add("framebuffer (chosen/atag,videolfb): base $(Hex (LE64 $b $o)) size $(Hex (LE32 $b ($o+16))) fps $(LE32 $b ($o+12)) lcm_found $(LE32 $b ($o+8)) lcm `"$name`"")
			}
			if ($cur.ContainsKey('compatible')) {
				$c = [Text.Encoding]::ASCII.GetString($b, $cur['compatible'][0], $cur['compatible'][1])
				if ($c -match 'ramoops') {
					$r = $cur['reg']; $vals = @(); for ($i = 0; $i -lt $r[1]; $i += 4) { $vals += Hex (BE32 $b ($r[0] + $i)) }
					$extra = @(); foreach ($k in 'record-size', 'console-size', 'ftrace-size', 'pmsg-size') {
						if ($cur.ContainsKey($k)) { $extra += "$k $(Hex (BE32 $b $cur[$k][0]))" } }
					$lines.Add("ramoops ($p): reg $($vals -join ' ') $($extra -join ', ')")
				}
			}
			if ($p -like '/reserved-memory/*' -and $cur.ContainsKey('reg') -and $cur['reg'][1] -ge 16) {
				$o = $cur['reg'][0]
				$base = ([uint64](BE32 $b $o) -shl 32) -bor (BE32 $b ($o+4)); $size = ([uint64](BE32 $b ($o+8)) -shl 32) -bor (BE32 $b ($o+12))
				$lines.Add("reserved $($path[$path.Count-1]): $(Hex $base) + $(Hex $size)")
			}
			if ($p -like '/memory*' -and $cur.ContainsKey('reg')) {
				$o = $cur['reg'][0]; $n = $cur['reg'][1]
				for ($i = 0; $i + 16 -le $n; $i += 16) {
					$base = ([uint64](BE32 $b ($o+$i)) -shl 32) -bor (BE32 $b ($o+$i+4)); $size = ([uint64](BE32 $b ($o+$i+8)) -shl 32) -bor (BE32 $b ($o+$i+12))
					$lines.Add("memory: $(Hex $base) + $(Hex $size)")
				}
			}
			$path.RemoveAt($path.Count - 1); $stack.RemoveAt($stack.Count - 1)
			if ($stack.Count -gt 0) { $props = $stack[$stack.Count - 1] }
		} elseif ($t -eq 4) { continue } else { break }
	}
	if (-not ($lines | Where-Object { $_ -like 'framebuffer*' })) { $lines.Insert(0, 'framebuffer: NO hay chosen/atag,videolfb') }
	if (-not ($lines | Where-Object { $_ -like 'ramoops*' })) { $lines.Insert(0, 'ramoops: NO aparece en el device tree de LK') }
} catch { $lines.Add("device tree: no se pudo leer ($($_.Exception.Message))") }
foreach ($l in Get-Content (Join-Path $Probe 'iomem.txt')) { if ($l -match 'Kernel code|System RAM') { $lines.Add('iomem: ' + $l.Trim()) } }
$lines | Set-Content -Encoding ASCII (Join-Path $Probe 'summary.txt')
Write-Host ''
$lines | ForEach-Object { Write-Host "  $_" }

$zip = Join-Path $Root 'hakios-probe.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $Probe '*') -DestinationPath $zip
Write-Host ''
Say "LISTO. Copias en: $Backup  (guárdalas también en otro sitio)"
Say "Envía en el hilo este archivo: $zip  (no lo subas a ningún repo público: lleva el device tree del fabricante)"
Start-Process explorer.exe "/select,`"$zip`""
Read-Host 'Pulsa Enter para salir' | Out-Null
