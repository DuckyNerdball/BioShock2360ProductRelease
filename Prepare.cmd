@echo off
rem RecompLauncher setup step (recomplauncher.json "prepare"): builds the game from your own disc.
rem Runs Install.ps1 with no prompts; it asks for administrator approval once.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Launcher %*
exit /b %ERRORLEVEL%
