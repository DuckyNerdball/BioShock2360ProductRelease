# Assemble a LOCAL folder that RecompLauncher can add as a build (Settings > Developers > Developer tools > "Add a build").
#   .\tools\package_local.ps1                 # -> out\recomplauncher\  (bs2start.exe, bioshock2.exe, mp\bioshock2mp.exe, DLLs, recomplauncher.json)
# NEVER upload this folder or attach it to a release: bioshock2.exe is code generated from YOUR disc. This project
# distributes scripts and source only; every player builds their own copy. out\ is gitignored for that reason.
param([string]$Out, [string]$Preset = "win-amd64-relwithdebinfo")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $Root "out\recomplauncher" }
$Bin = Join-Path $Root "app\out\build\$Preset"
if (-not (Test-Path (Join-Path $Bin "bioshock2.exe"))) { throw "No built game at $Bin. Build it first: .\go.ps1" }
New-Item -ItemType Directory $Out, (Join-Path $Out "run"), (Join-Path $Out "userdata") -Force | Out-Null
Copy-Item (Join-Path $Bin "bioshock2.exe") $Out -Force
if (-not (Test-Path (Join-Path $Bin "bs2start.exe"))) { throw "bs2start.exe is not built yet. Build again: .\go.ps1" }
Copy-Item (Join-Path $Bin "bs2start.exe") $Out -Force
Copy-Item (Join-Path $Bin "*.dll") $Out -Force
Copy-Item (Join-Path $Root "recomplauncher.json") $Out -Force
# Multiplayer (offline, vs bots): the newest build of the dev project next door (..\bioshock2mp-recomp) or of mp\ here.
$MpRoot = @((Join-Path (Split-Path -Parent $Root) "bioshock2mp-recomp"), (Join-Path $Root "mp")) |
    Where-Object { Test-Path (Join-Path $_ "app\out\build\$Preset\bioshock2mp.exe") } |
    Sort-Object { (Get-Item (Join-Path $_ "app\out\build\$Preset\bioshock2mp.exe")).LastWriteTime } -Descending | Select-Object -First 1
if ($MpRoot) {
    $MpBin = Join-Path $MpRoot "app\out\build\$Preset"; $MpOut = Join-Path $Out "mp"
    New-Item -ItemType Directory $MpOut, (Join-Path $MpOut "run") -Force | Out-Null
    Copy-Item (Join-Path $MpBin "bioshock2mp.exe") $MpOut -Force
    Copy-Item (Join-Path $MpBin "*.dll") $MpOut -Force
    foreach ($f in "mp_bots.ini", "mp_onspawn.txt") {   # bot settings: your tuned run\ copy if there is one, else the defaults
        $src = @((Join-Path $MpRoot "run\$f"), (Join-Path $MpRoot "config\$f")) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($src) { Copy-Item $src (Join-Path $MpOut "run") -Force }
    }
    # Your multiplayer profile (rank, unlocks) is kept in run\userdata. Bring the dev one over once; never overwrite.
    $prof = Join-Path $MpRoot "run\userdata\DefaultProfile.bin"; $profOut = Join-Path $MpOut "run\userdata"
    if ((Test-Path $prof) -and -not (Test-Path (Join-Path $profOut "DefaultProfile.bin"))) {
        New-Item -ItemType Directory $profOut -Force | Out-Null; Copy-Item $prof $profOut
    }
    Write-Host "Multiplayer build from: $MpBin" -ForegroundColor Green
} else {
    Write-Host "No multiplayer build found - the Multiplayer tile will say so. Build it: .\mp\tools\mp_init.ps1 -NoImport" -ForegroundColor Yellow
}
Write-Host "Local RecompLauncher build: $Out" -ForegroundColor Green
Write-Host "In RecompLauncher: Settings > Developers > Developer tools > Add a build > pick that folder."
Write-Host "When it asks for the game, pick your extracted disc folder: $(Join-Path $Root 'game_bs2')"
Write-Host "Do not upload this folder: it contains code generated from your disc." -ForegroundColor Yellow
