# Build the app (VS dev shell) and launch the BioShock 2 MP recomp. From the repo root:  .\go   |  .\go -NoBuild
param([switch]$NoBuild)
$Root = $PSScriptRoot
. (Join-Path $Root "tools\vsenv.ps1")
if (-not $NoBuild) {
    Get-ChildItem (Join-Path $Root "mp_src") -Include *.cpp, *.h -File -Recurse | Copy-Item -Destination (Join-Path $Root "app\src") -Force
    Push-Location (Join-Path $Root "app")
    try { cmake --build --preset win-amd64-relwithdebinfo 2>&1 | Tee-Object -FilePath (Join-Path $Root "logs\build_last.log"); if ($LASTEXITCODE -ne 0) { throw "Build failed (see logs\build_last.log)" } } finally { Pop-Location }
}
& (Join-Path $Root "tools\play.ps1") -Project bioshock2mp -GameDir game_bs2 -Extra "--fake_live_signin=true" @args   # SDK cvar (xam_user.cpp patch 11-07)
