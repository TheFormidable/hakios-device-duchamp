@echo off
rem Paso 2 del port de hakiOS: flashear y probar el loader (sin XNU), o restaurar el boot_a original.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hakios-paso2.ps1"
if errorlevel 1 pause
