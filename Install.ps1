#Requires -Version 5.1
<#
BioShock 2 360 Recomp - one-shot installer for a fresh Windows PC.

  Right-click > "Run with PowerShell", or:
    powershell -ExecutionPolicy Bypass -File .\Install.ps1 [-Iso <path to your BioShock 2 .iso>] [-NoMultiplayer]

What it does (safe to re-run; every step skips what is already done):
  1. Installs build tools: Git, CMake, Ninja, Python, Visual Studio 2022 C++ + Clang, LLVM 20+ (winget)
  2. Gets extract-xiso and extracts YOUR disc image into game_bs2\
  3. Checks Bioshock2_Xenon.xex / Bioshock2MP_Xenon.xex are the exact builds this project targets (SHA-1)
  4. Builds the patched ReXGlue SDK, generates the C++ from your XEX, compiles bioshock2.exe
  5. Builds the offline multiplayer (private matches vs AI bots) from Bioshock2MP_Xenon.xex -> mp\
  6. Puts a "BioShock 2 Recomp" launcher shortcut on your desktop

Needs: Windows 10/11 x64, ~60 GB free, internet, admin rights (Visual Studio), 1-2 hours the first time.
Nothing from the game is downloaded or shipped: you supply your own legally dumped disc image.
#>
param(
    [string]$Iso,
    [switch]$SkipPrereqs,
    [switch]$NoShortcut,
    [switch]$NoMultiplayer
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"
$Root = $PSScriptRoot
$ExpectedXexSha1 = "b4196d2bd339ff5e80ee1ce9100cb288bac8a13f"   # BioShock 2 (360) Bioshock2_Xenon.xex, TitleID 54540861
$ExpectedMpSha1  = "d5b5d391a6aa06b23c33dd175e1fa20416ee3736"   # Bioshock2MP_Xenon.xex (same disc)
$BinDir = Join-Path $Root "tools\bin"

function Step($m) { Write-Host "`n=== $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Die($m)  { Write-Host "`n  [STOP] $m`n" -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

# --- elevate (Visual Studio install and SDK symlinks need it)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "Restarting as Administrator..."
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($Iso) { $a += @("-Iso", "`"$Iso`"") }
    if ($SkipPrereqs) { $a += "-SkipPrereqs" }; if ($NoShortcut) { $a += "-NoShortcut" }; if ($NoMultiplayer) { $a += "-NoMultiplayer" }
    Start-Process powershell -Verb RunAs -ArgumentList $a
    exit
}
Set-Location $Root
$t0 = Get-Date

Step "1. Build tools"
if ($SkipPrereqs) { Ok "skipped (-SkipPrereqs)" }
else {
    & (Join-Path $Root "tools\bootstrap.ps1") -SkipGhidra -SkipXenia -SkipDiscs -SkipSetup -MinFreeGB 60
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}

Step "2. Your game files"
$xex = Join-Path $Root "game_bs2\Bioshock2_Xenon.xex"
$mpXex = Join-Path $Root "game_bs2\Bioshock2MP_Xenon.xex"
if (Test-Path $xex) { Ok "game_bs2\ already extracted" }
else {
    if (-not $Iso) {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select YOUR BioShock 2 (Xbox 360) disc image"
        $dlg.Filter = "Xbox 360 disc image (*.iso)|*.iso"
        if ($dlg.ShowDialog() -ne "OK") { Die "No disc image selected." }
        $Iso = $dlg.FileName
    }
    if (-not (Test-Path $Iso)) { Die "Not found: $Iso" }
    $xiso = Get-ChildItem $BinDir -Recurse -Filter "extract-xiso.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $xiso) {
        Write-Host "  Downloading extract-xiso (XboxDev/extract-xiso, open source)..."
        $rel = Invoke-RestMethod -Headers @{ "User-Agent" = "bioshock-recomp-install" } "https://api.github.com/repos/XboxDev/extract-xiso/releases/latest"
        $asset = $rel.assets | Where-Object { $_.name -match "win" -and $_.name -match "\.zip$" } | Select-Object -First 1
        if (-not $asset) { Die "Couldn't find a Windows extract-xiso download. Get it from https://github.com/XboxDev/extract-xiso/releases, put extract-xiso.exe in tools\bin\ and re-run." }
        $zip = Join-Path $env:TEMP $asset.name
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing
        New-Item -ItemType Directory $BinDir -Force | Out-Null
        Expand-Archive $zip -DestinationPath $BinDir -Force; Remove-Item $zip
        $xiso = Get-ChildItem $BinDir -Recurse -Filter "extract-xiso.exe" | Select-Object -First 1
        if (-not $xiso) { Die "extract-xiso.exe not found after download." }
    }
    Ok "extract-xiso: $($xiso.FullName)"
    & (Join-Path $Root "tools\inspect_disc.ps1") -Iso $Iso -Name bs2 -ExtractXiso $xiso.FullName
    if (-not (Test-Path $xex)) { Die "Extraction finished but game_bs2\Bioshock2_Xenon.xex is missing. Is this the BioShock 2 (Xbox 360) disc?" }
}

Step "3. Check it's the supported build"
$h = (Get-FileHash $xex -Algorithm SHA1).Hash.ToLower()
if ($h -ne $ExpectedXexSha1) {
    Die "Bioshock2_Xenon.xex SHA-1 is $h`n  Supported: $ExpectedXexSha1 (BioShock 2 360, TitleID 54540861).`n  Every address in this project is tied to that exact file. Other regions/versions are not supported yet."
}
Ok "Bioshock2_Xenon.xex matches ($h)"
$mpOk = $false
if ($NoMultiplayer) { Ok "multiplayer skipped (-NoMultiplayer)" }
elseif (-not (Test-Path $mpXex)) { Write-Host "  [skip] Bioshock2MP_Xenon.xex not on this disc: no multiplayer" -ForegroundColor Yellow }
else {
    $hm = (Get-FileHash $mpXex -Algorithm SHA1).Hash.ToLower()
    if ($hm -eq $ExpectedMpSha1) { Ok "Bioshock2MP_Xenon.xex matches ($hm)"; $mpOk = $true }
    else { Write-Host "  [skip] Bioshock2MP_Xenon.xex SHA-1 $hm is not the supported $ExpectedMpSha1 - no multiplayer" -ForegroundColor Yellow }
}

Step "4. Build (SDK + code generation + compile; 20-60 min the first time)"
. (Join-Path $Root "tools\vsenv.ps1")
& (Join-Path $Root "tools\setup.ps1")
Push-Location (Join-Path $Root "app")
try {
    function Run([string[]]$a) { & cmake @a; if ($LASTEXITCODE -ne 0) { throw "cmake $($a -join ' ') failed" } }
    Run @("--preset", "win-amd64-relwithdebinfo")
    Run @("--build", "--preset", "win-amd64-relwithdebinfo", "--target", "bioshock2_codegen")
    Run @("--preset", "win-amd64-relwithdebinfo")   # re-configure: generated sources list only exists after codegen
    Run @("--build", "--preset", "win-amd64-relwithdebinfo")
} catch { Pop-Location; Die "Build failed: $($_.Exception.Message)" }
Pop-Location
$exe = Join-Path $Root "app\out\build\win-amd64-relwithdebinfo\bioshock2.exe"
if (-not (Test-Path $exe)) { Die "bioshock2.exe was not produced." }
Ok "bioshock2.exe built"

Step "5. Offline multiplayer (private matches vs AI bots; 10-30 min)"
if (-not $mpOk) { Ok "skipped" }
else {
    try { & (Join-Path $Root "mp\tools\mp_init.ps1") -NoImport -RexBin (Join-Path $Root ".deps\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe") }
    catch { Write-Host "  [warn] multiplayer build failed: $($_.Exception.Message)`n  Single player is fine. Retry later: mp\tools\mp_init.ps1 -NoImport" -ForegroundColor Yellow }
    if (Test-Path (Join-Path $Root "mp\app\out\build\win-amd64-relwithdebinfo\bioshock2mp.exe")) { Ok "bioshock2mp.exe built" }
}

Step "6. Launcher shortcut"
if ($NoShortcut) { Ok "skipped" }
else {
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "BioShock 2 Recomp.lnk"
    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $Root "BioShock2.cmd"
    $s.WorkingDirectory = $Root
    $s.Description = "BioShock 2 360 Recomp launcher"
    $s.Save()
    Ok "Desktop shortcut: $lnk"
}

Write-Host ("`nDone in {0:N0} min. Start the game with the 'BioShock 2 Recomp' desktop shortcut (or BioShock2.cmd)." -f ((Get-Date) - $t0).TotalMinutes) -ForegroundColor Green
Read-Host "Press Enter to close"
