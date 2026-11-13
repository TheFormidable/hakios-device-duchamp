@echo off
rem hakiOS en el POCO X6 Pro: copia el firmware de la GPU (mali_csffw.bin) de TU Android a la carpeta fw-gpu, junto a este
rem script. Solo lee del telefono. Necesita Android arrancado, depuracion USB y root (Magisk). El archivo es de MediaTek/ARM:
rem no se sube a ningun repositorio, solo se mete en tu propia imagen.
setlocal
set ADB=adb
where adb >nul 2>nul || set ADB=C:\platform-tools\adb.exe
if not exist "%ADB%" if "%ADB%" neq "adb" (echo No encuentro adb ^(ni en el PATH ni en C:\platform-tools^). & pause & exit /b 1)
"%ADB%" get-state >nul 2>nul || (echo El telefono no aparece en adb: arranca Android y acepta la depuracion USB. & pause & exit /b 1)
set OUT=%~dp0fw-gpu
if not exist "%OUT%" mkdir "%OUT%"
echo Buscando mali_csffw en el telefono...
"%ADB%" shell "su -c 'rm -rf /data/local/tmp/hakios-gpu; mkdir -p /data/local/tmp/hakios-gpu; find /vendor/firmware /vendor/etc/firmware /odm/firmware /vendor/firmware_mnt /system/vendor/firmware -maxdepth 2 -type f -iname mali_csffw* 2>/dev/null | while read f; do echo \"  $f\"; cp \"$f\" /data/local/tmp/hakios-gpu/; done; chmod 644 /data/local/tmp/hakios-gpu/*'"
"%ADB%" pull /data/local/tmp/hakios-gpu/. "%OUT%" >nul 2>nul
"%ADB%" shell "su -c 'rm -rf /data/local/tmp/hakios-gpu'"
if not exist "%OUT%\mali_csffw.bin" (echo No salio mali_csffw.bin: revisa que Magisk le de root a la shell. & pause & exit /b 1)
for %%F in ("%OUT%\mali_csffw*") do echo Copiado: %%~nxF, %%~zF bytes
certutil -hashfile "%OUT%\mali_csffw.bin" SHA256 | findstr /v ":"
echo.
echo Listo. Mandame el archivo fw-gpu\mali_csffw.bin en el hilo.
pause
