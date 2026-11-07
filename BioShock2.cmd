@echo off
rem Opens the BioShock 2 Recomp launcher window.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0Launcher.ps1"
