# Run -> find "unregistered function" -> declare it -> codegen/configure/build -> run again.
# Run from the repo root INSIDE the VS dev shell with LLVM first on PATH (see README).
#   .\tools\fix_loop.ps1 -MaxIterations 20 -RunSeconds 60
# Stops on: a different crash, a build/codegen failure, a repeat address, or the game surviving -RunSeconds.
param(
    [int]$MaxIterations = 20,
    [int]$RunSeconds    = 60,
    [string]$Preset     = "win-amd64-relwithdebinfo",
    [ValidateSet('Pure','Crisp','Sharp','Clean','Ultra','Cinematic','BS2')][string]$Look,
    [switch]$RenderDoc,  # run under RenderDoc: F10 marks the log AND captures that frame to captures\
    [string]$Project    = "bioshock",     # bioshock2-recomp: -Project bioshock2 -GameDir game_bs2
    [string]$GameDir    = "game_base1"
)
. (Join-Path $PSScriptRoot 'look.ps1')
$ErrorActionPreference = "Stop"
$Root     = Split-Path -Parent $PSScriptRoot
$App      = Join-Path $Root "app"
$Manifest = Join-Path $App "${Project}_manifest.toml"
$Exe      = Join-Path $App "out\build\$Preset\$Project.exe"
$LogDir   = Join-Path $Root "logs"
$UserData = Join-Path $Root "run\userdata"
New-Item -ItemType Directory $LogDir, $UserData -Force | Out-Null
if (-not $env:VCToolsInstallDir) { throw "Run inside the VS dev shell (Enter-VsDevShell) with LLVM first on PATH." }

function Invoke-Build([string]$Tag) {
    Push-Location $App
    try {
        foreach ($step in @(
            @("--build", "--preset", $Preset, "--target", "${Project}_codegen"),
            @("--preset", $Preset),
            @("--build", "--preset", $Preset))) {
            $log = Join-Path $LogDir "build_$Tag.log"
            cmd /c "cmake $($step -join ' ') 2>&1" | Out-File -Append -Encoding utf8 $log
            if ($LASTEXITCODE -ne 0) { throw "cmake $($step -join ' ') failed. See $log" }
        }
    } finally { Pop-Location }
}

function Get-DeclaredFunctions {
    $set = @{}
    foreach ($m in [regex]::Matches((Get-Content $Manifest -Raw), '"(0x[0-9A-Fa-f]{8})"\s*=')) { $set[$m.Groups[1].Value.ToUpper()] = $true }
    $set
}

function Add-Function([string]$Addr, [string]$Note) {
    $raw = Get-Content $Manifest -Raw
    if ($raw -notmatch '(?m)^\[entrypoint\.functions\]') { Add-Content $Manifest "`n[entrypoint.functions]" }
    # [entrypoint.functions] must remain the LAST table in the manifest for appends to land inside it.
    $last = ([regex]::Matches($raw, '(?m)^\[[^\]]+\]') | Select-Object -Last 1).Value
    if ($last -and $last -ne '[entrypoint.functions]') { throw "Manifest's last table is $last, not [entrypoint.functions]. Move it to the end, then re-run." }
    Add-Content $Manifest "# $(Get-Date -Format yyyy-MM-dd): $Note`n`"$Addr`" = {}"
}

for ($i = 1; $i -le $MaxIterations; $i++) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $runLog = Join-Path $LogDir "run_$stamp.log"
    Write-Host "`n=== Iteration $i : run -> $runLog" -ForegroundColor Cyan
    if (-not $Look) { $Look = if ($Project -eq 'bioshock2') { 'BS2' } else { 'Pure' } }
    $LL = $Looks[$Look]
    $gameArgs = @(Get-LookArgs $LL -SkipComposite ([bool]$LL.SkipComposite)) + @("--game_data_root", "`"$Root\$GameDir`"", "--user_data_root", "`"$UserData`"", "--log_file", "`"$runLog`"")
    if ($RenderDoc) {
        New-Item -ItemType Directory (Join-Path $Root "captures") -Force | Out-Null
        $p = Start-Process "C:\Program Files\RenderDoc\renderdoccmd.exe" -PassThru -ArgumentList (@("capture", "-w", "-d", "`"$Root`"", "-c", "`"$Root\captures\loop_$stamp`"", "`"$Exe`"") + $gameArgs)
    } else {
        $p = Start-Process $Exe -PassThru -ArgumentList $gameArgs
    }
    if (-not $p.WaitForExit($RunSeconds * 1000)) {
        Write-Host "No crash in $RunSeconds s. Keep playing; press F10 on any texture/visual glitch. Quit the game to get the report." -ForegroundColor Green
        $p.WaitForExit()
    }
    Start-Sleep -Milliseconds 500
    & (Join-Path $PSScriptRoot 'report.ps1') $runLog
    if ($RenderDoc) { Get-ChildItem (Join-Path $Root "captures") -Filter "loop_$stamp*.rdc" -ErrorAction SilentlyContinue | ForEach-Object { "  capture: $($_.Name)" } }
    $text = if (Test-Path $runLog) { Get-Content $runLog -Raw } else { "" }
    $m = [regex]::Match($text, 'unregistered function at guest address (0x[0-9A-Fa-f]{8})')
    if (-not $m.Success) {
        Write-Host "`nExited without an 'unregistered function' error (you quit, or a different problem - see report above)." -ForegroundColor Yellow
        break
    }
    $addr = $m.Groups[1].Value.ToUpper() -replace '^0X', '0x'
    if ((Get-DeclaredFunctions).ContainsKey($addr)) {
        Write-Host "$addr is already declared but still unregistered: needs manual analysis (mid-function target or data). Stopping." -ForegroundColor Red
        break
    }
    Write-Host "Declaring $addr and rebuilding ..." -ForegroundColor Yellow
    Add-Function $addr "indirect-call target found at runtime (fix_loop)"
    Add-Content (Join-Path $Root "docs\STATUS.md") "- $(Get-Date -Format yyyy-MM-dd) - $addr - runtime: call to unregistered function - declared in [entrypoint.functions] (fix_loop)"
    Invoke-Build $stamp
}
Write-Host "`nDone. Review: git diff app\${Project}_manifest.toml docs\STATUS.md"

