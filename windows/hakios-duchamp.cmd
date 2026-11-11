@echo off
rem Doble clic: copia de seguridad y sondeo del POCO X6 Pro para el port de hakiOS (solo lee el telefono).
rem Lanza hakios-duchamp.ps1 sin cambiar la politica de ejecucion de Windows.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hakios-duchamp.ps1"
if errorlevel 1 pause
