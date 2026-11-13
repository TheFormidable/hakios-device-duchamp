@echo off
rem hakiOS en el POCO X6 Pro: system.apfs a la particion xnusys (Android arrancado, con root). Arrastra system.apfs encima, o doble clic.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hakios-sistema.ps1" "%~1"
if errorlevel 1 pause
