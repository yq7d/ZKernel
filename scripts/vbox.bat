@echo off
rem double click, or: vbox.bat path\to\zkernel.iso [-Disk disk.img] [-Uefi]
rem runs vbox.ps1 without changing the execution policy
if "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vbox.ps1"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vbox.ps1" -Iso %*
)
if errorlevel 1 pause
