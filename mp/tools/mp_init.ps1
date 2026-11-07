# One-time scaffold + first build of the BioShock 2 MULTIPLAYER recomp (offline research build).
# Run from the repo root in a normal PowerShell (it enters the VS dev shell itself):
#   .\tools\mp_init.ps1              # junction, rexglue init, overlay mp_src\, codegen, verify hook addresses, vtable import, build
#   .\tools\mp_init.ps1 -NoHooks     # same, but MP_SPAWNLOG=OFF (clean boot, no VM hooks)
#   .\tools\mp_init.ps1 -NoImport    # skip the vtable re-scan (players: the manifest already has the functions)
# Re-runnable: skips steps already done. Game data is a junction to ..\bioshock2-recomp\game_bs2 (never copied/committed).
param([switch]$NoHooks, [switch]$NoImport, [string]$Preset = "win-amd64-relwithdebinfo", [string]$RexBin = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$App  = Join-Path $Root "app"
# Two layouts: mp\ inside the BioShock 2 repo (release / players), or a sibling ..\bioshock2mp-recomp (dev).
$Parent = Split-Path -Parent $Root
$Sp = if (Test-Path (Join-Path $Parent "tools\setup.ps1")) { $Parent } else { Join-Path $Parent "bioshock2-recomp" }
if (-not $RexBin) {
    $RexBin = @((Join-Path $Sp ".deps\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe"),
                "E:\Projects\BioshockProject\BioShockRaptureEdition\bioshock-recomp\.deps\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe") |
              Where-Object { Test-Path $_ } | Select-Object -First 1
}
$Logs = Join-Path $Root "logs"
New-Item -ItemType Directory $Logs, (Join-Path $Root "run\userdata"), (Join-Path $Root "docs") -Force | Out-Null
# run\ is gitignored: seed the bot/onspawn config from config\ the first time
foreach ($f in "mp_bots.ini", "mp_onspawn.txt") { $src = Join-Path $Root "config\$f"; $dst = Join-Path $Root "run\$f"
    if ((Test-Path $src) -and -not (Test-Path $dst)) { Copy-Item $src $dst } }
. (Join-Path $PSScriptRoot "vsenv.ps1")
function Invoke-Native([string]$Exe, [string[]]$CmdArgs, [string]$Log) {
    Write-Host "> $Exe $($CmdArgs -join ' ')" -ForegroundColor DarkGray
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { if ($Log) { & $Exe @CmdArgs 2>&1 | Tee-Object -Append -FilePath $Log } else { & $Exe @CmdArgs }; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "$Exe $($CmdArgs -join ' ') failed (exit $code)$(if ($Log) { " - see $Log" })" }
}

# 1. game data junction
$Game = Join-Path $Root "game_bs2"
if (-not (Test-Path $Game)) {
    New-Item -ItemType Junction -Path $Game -Target (Join-Path $Sp "game_bs2") | Out-Null
    Write-Host "junction game_bs2 -> $Sp\game_bs2"
}
$Xex = Join-Path $Game "Bioshock2MP_Xenon.xex"
if (-not (Test-Path $Xex)) { throw "MP XEX not found: $Xex" }

# 2. rexglue init (once)
if (-not (Test-Path (Join-Path $App "bioshock2mp_manifest.toml"))) {
    if (-not $RexBin -or -not (Test-Path $RexBin)) { throw "rexglue.exe not found (run ..\tools\setup.ps1 first, or pass -RexBin)" }
    Invoke-Native $RexBin @("init", "--project-name", "bioshock2mp", "--xex-path", $Xex, "--game-root", $Game, "--project-root", $App)
}

# 3. our sources over the template (mp_src\ is the source of truth; app\src is a copy)
New-Item -ItemType Directory (Join-Path $App "src") -Force | Out-Null
Copy-Item (Join-Path $Root "mp_src\CMakeLists.txt") $App -Force
Get-ChildItem (Join-Path $Root "mp_src") -Include *.cpp, *.h -File -Recurse | Copy-Item -Destination (Join-Path $App "src") -Force
if (-not (Test-Path (Join-Path $Root "docs\STATUS.md"))) { "# bioshock2mp STATUS (append-only: declared functions and why)" | Set-Content (Join-Path $Root "docs\STATUS.md") }

# 4. configure + codegen + reconfigure
$Bl = Join-Path $Logs "build_init_$(Get-Date -Format yyyyMMdd_HHmmss).log"
$hooks = if ($NoHooks) { "OFF" } else { "ON" }
$RexCfg = Join-Path $Sp ".deps\rexglue-sdk\out\install\win-amd64\lib\cmake\rexglue"   # repo SDK (players); dev uses the registry
$RexPin = @(); if (Test-Path $RexCfg) { $RexPin = @("-Drexglue_DIR=$RexCfg") }
Push-Location $App
try {
    Invoke-Native cmake (@("--preset", $Preset, "-DMP_SPAWNLOG=$hooks") + $RexPin) $Bl
    Invoke-Native cmake @("--build", "--preset", $Preset, "--target", "bioshock2mp_codegen") $Bl
    Invoke-Native cmake (@("--preset", $Preset, "-DMP_SPAWNLOG=$hooks") + $RexPin) $Bl
} finally { Pop-Location }

# 5. the hook addresses must be real function starts in the MP codegen (they were matched from SP by signature)
$funcsH = Get-ChildItem (Join-Path $App "generated") -Recurse -Filter "bioshock2mp_funcs.h" | Select-Object -First 1
if (-not $funcsH) { throw "bioshock2mp_funcs.h not found under app\generated - codegen failed? See $Bl" }
$txt = Get-Content $funcsH.FullName -Raw
$want = [ordered]@{ "828209B0" = "ProcessEvent"; "82821AC0" = "CallFunction"; "8281F318" = "ProcessInternal"; "828049E0" = "FName->FString" }
$bad = @()
foreach ($a in $want.Keys) {
    $ok = $txt -match "DECLARE_REX_FUNC\(sub_$a\)"
    Write-Host ("{0,-16} {1}  {2}" -f $want[$a], $a, $(if ($ok) { "OK" } else { "MISSING" })) -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
    if (-not $ok) { $bad += $a }
}
if ($bad -and -not $NoHooks) { throw "Hook address(es) $($bad -join ', ') are not function starts in the MP codegen. Re-run with -NoHooks to get a clean boot, and paste this output to Claude." }

# 6. bulk-declare vtable leaf functions the analyzer misses (BS2 SP lesson), then codegen + configure + build
if ($NoImport) {   # players: the manifest already carries every declared function, so just build
    Push-Location $App
    try { Invoke-Native cmake @("--build", "--preset", $Preset) $Bl } finally { Pop-Location }
} else { & (Join-Path $PSScriptRoot "import_functions.ps1") -Build }
Copy-Item $Bl (Join-Path $Logs "build_last.log") -Force
if ($NoImport) { Write-Host "Multiplayer built." -ForegroundColor Green; return }   # players: no dev hints
Write-Host "`nDone. Next:" -ForegroundColor Green
Write-Host "  .\tools\fix_loop.ps1 -RunSeconds 90       # boot to the MP front end, declaring missing functions as it goes"
Write-Host "  .\go.ps1 -NoBuild                          # play; logs\mp_*.log / mp_vocab.tsv / mp_snap_*.txt"
