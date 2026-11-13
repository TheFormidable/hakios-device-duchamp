@echo off
rem Paso 3 del port de hakiOS: flashear la imagen con XNU, arrancar XNU una vez y traer su registro, o restaurar.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hakios-paso3.ps1"
if errorlevel 1 pause
