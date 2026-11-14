# Build the app (VS dev shell) and launch the BioShock 2 MP recomp. From the repo root:  .\go   |  .\go -NoBuild
param([switch]$NoBuild, [switch]$Clean)   # -Clean: no glow at all (default: the game's bloom at 5%, edge blur off)
# Picture: the game's bloom pass (composite PS 4B7A33E482E2DF58, strength c1, edge blur c4) hazes the screen at 2x.
$H = "4B7A33E482E2DF58"
$Pic = if ($Clean) { @("--sharp_composite_ps=$H", "--ps_const_mul=$H`:1:0:0,$H`:1:1:0,$H`:1:2:0") }
       else { @("--ps_const_mul=$H`:1:0:0.125,$H`:1:1:0.125,$H`:1:2:0.125,$H`:4:0:0,$H`:4:1:0,$H`:4:2:0") }
$Root = $PSScriptRoot
. (Join-Path $Root "tools\vsenv.ps1")
if (-not $NoBuild) {
    Get-ChildItem (Join-Path $Root "mp_src") -Include *.cpp, *.h -File -Recurse | Copy-Item -Destination (Join-Path $Root "app\src") -Force
    Push-Location (Join-Path $Root "app")
    try { cmake --build --preset win-amd64-relwithdebinfo 2>&1 | Tee-Object -FilePath (Join-Path $Root "logs\build_last.log"); if ($LASTEXITCODE -ne 0) { throw "Build failed (see logs\build_last.log)" } } finally { Pop-Location }
}
& (Join-Path $Root "tools\play.ps1") -Project bioshock2mp -GameDir game_bs2 -Extra (@("--fake_live_signin=true") + $Pic) @args   # SDK cvar (xam_user.cpp patch 11-07)
