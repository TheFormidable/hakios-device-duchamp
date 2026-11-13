@echo off
rem hakiOS en el POCO X6 Pro: la particion xnusys (1 leer y respaldar, 2 escribir la tabla nueva y borrar userdata).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hakios-xnusys.ps1"
if errorlevel 1 pause
