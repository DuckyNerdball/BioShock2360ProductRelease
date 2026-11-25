#Requires -Version 5.1
<#
BioShock 2 360 Recomp - one-shot installer for a fresh Windows PC.

  Right-click > "Run with PowerShell", or:
    powershell -ExecutionPolicy Bypass -File .\Install.ps1 [-Iso <path to your BioShock 2 .iso>] [-NoMultiplayer]

  RecompLauncher runs it for you (Prepare.cmd -> Install.ps1 -Launcher): pick your .iso, press Set Up, approve the
  administrator prompt once. In -Launcher mode nothing is asked and nothing waits for a key; progress goes to stdout as
  "progress <0-1> <text>" lines.

What it does (safe to re-run; every step skips what is already done):
  1. Installs build tools: Git, CMake, Ninja, Python, Visual Studio 2022 C++ + Clang, LLVM 20+ (winget)
  2. Gets extract-xiso and extracts YOUR disc image into game_bs2\
  3. Checks Bioshock2_Xenon.xex / Bioshock2MP_Xenon.xex are the exact builds this project targets (SHA-1)
  4. Builds the patched ReXGlue SDK, generates the C++ from your XEX, compiles bioshock2.exe
  5. Builds the offline multiplayer (private matches vs AI bots) from Bioshock2MP_Xenon.xex -> mp\
  6. Puts a "BioShock 2 Recomp" launcher shortcut on your desktop (not in -Launcher mode)

Needs: Windows 10/11 x64, ~60 GB free, internet, admin rights (Visual Studio), 1-2 hours the first time.
Nothing from the game is downloaded or shipped: you supply your own legally dumped disc image.

Exit codes (-Launcher): 0 ok, 1 unexpected error, 2 no disc image given, 3 Visual Studio licence not accepted,
4 build failed, 5 not the supported disc, 6 administrator prompt declined.
#>
param(
    [string]$Iso,
    [switch]$SkipPrereqs,
    [switch]$NoShortcut,
    [switch]$NoMultiplayer,
    [switch]$Launcher,          # run by RecompLauncher: no prompts, progress lines, exit codes
    [switch]$AcceptVsLicense,   # RecompLauncher setup option: OK to install Visual Studio 2022 Community
    [string]$ProgressFile       # internal: the elevated copy reports progress through this file
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"
$Root = $PSScriptRoot
$ExpectedXexSha1 = "b4196d2bd339ff5e80ee1ce9100cb288bac8a13f"   # BioShock 2 (360) Bioshock2_Xenon.xex, TitleID 54540861
$ExpectedMpSha1  = "d5b5d391a6aa06b23c33dd175e1fa20416ee3736"   # Bioshock2MP_Xenon.xex (same disc)
$BinDir = Join-Path $Root "tools\bin"
$xex = Join-Path $Root "game_bs2\Bioshock2_Xenon.xex"
$mpXex = Join-Path $Root "game_bs2\Bioshock2MP_Xenon.xex"
if (-not $Iso -and $env:BS_ISO) { $Iso = $env:BS_ISO }   # RecompLauncher passes the picked .iso this way (prepare.env)

function Report([double]$f, [string]$t) {
    if (-not $Launcher) { return }
    $line = "progress {0} {1}" -f $f.ToString([Globalization.CultureInfo]::InvariantCulture), $t
    if ($ProgressFile) { Add-Content -Path $ProgressFile -Value $line -Encoding ascii } else { [Console]::Out.WriteLine($line) }
}
function Step([string]$m, [double]$f) { Write-Host "`n=== $m" -ForegroundColor Cyan; Report $f ($m -replace '^\d+\.\s*', '') }
function Ok($m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Die([string]$m, [int]$code = 1) {
    Write-Host "`n  [STOP] $m`n" -ForegroundColor Red
    if ($Launcher) {
        if ($ProgressFile) { Add-Content -Path $ProgressFile -Value "error $m" -Encoding ascii; Add-Content -Path $ProgressFile -Value "exit $code" -Encoding ascii }
        else { [Console]::Out.WriteLine("error $m") }
        exit $code
    }
    Read-Host "Press Enter to close"; exit 1
}
function Test-VsCpp {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    [bool](& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}

# --- RecompLauncher: refuse early (before any admin prompt) when the step can't succeed
if ($Launcher -and -not $ProgressFile) {
    if (-not (Test-Path $xex) -and (-not $Iso -or -not (Test-Path $Iso))) { Die "No disc image: pick your BioShock 2 (Xbox 360) .iso in RecompLauncher." 2 }
    if (-not $SkipPrereqs -and -not $AcceptVsLicense -and -not (Test-VsCpp)) {
        Die "This PC has no Visual Studio C++ tools. Tick the Visual Studio licence in the setup options so it can be installed." 3
    }
}

# --- elevate (Visual Studio install and SDK symlinks need it)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($Iso) { $a += @("-Iso", "`"$Iso`"") }
    if ($SkipPrereqs) { $a += "-SkipPrereqs" }; if ($NoShortcut) { $a += "-NoShortcut" }; if ($NoMultiplayer) { $a += "-NoMultiplayer" }
    if (-not $Launcher) {
        Write-Host "Restarting as Administrator..."
        Start-Process powershell -Verb RunAs -ArgumentList $a
        exit
    }
    # RecompLauncher: run the elevated copy, relay its progress lines, keep the launcher fed while it works
    # (a step that prints nothing for an hour is treated as stuck), and hand back its exit code.
    $pf = Join-Path ([IO.Path]::GetTempPath()) ("bioshock2_setup_{0}.txt" -f $PID)
    Set-Content -Path $pf -Value "" -Encoding ascii
    $a += @("-Launcher", "-ProgressFile", "`"$pf`"")
    if ($AcceptVsLicense) { $a += "-AcceptVsLicense" }
    Report 0.01 "Waiting for the administrator prompt"
    try { $p = Start-Process powershell -Verb RunAs -ArgumentList $a -PassThru }
    catch { [Console]::Out.WriteLine("error The administrator prompt was declined."); exit 6 }
    $done = 0; $lastLine = "progress 0.02 Setting up"; $lastOut = Get-Date; $t0 = Get-Date; $childExit = $null
    while ($true) {
        $exited = $p.HasExited
        $lines = @(Get-Content -Path $pf -ErrorAction SilentlyContinue)
        for ($i = $done; $i -lt $lines.Count; $i++) {
            $l = $lines[$i]; if (-not $l) { continue }
            if ($l -match '^exit (\d+)$') { $childExit = [int]$Matches[1]; continue }
            [Console]::Out.WriteLine($l); $lastOut = Get-Date
            if ($l -like "progress *") { $lastLine = $l }
        }
        $done = $lines.Count
        if ($exited) { break }
        if (((Get-Date) - $lastOut).TotalSeconds -ge 60) {
            [Console]::Out.WriteLine(("{0} ({1:N0} min)" -f $lastLine, ((Get-Date) - $t0).TotalMinutes)); $lastOut = Get-Date
        }
        Start-Sleep -Seconds 2
    }
    Remove-Item $pf -ErrorAction SilentlyContinue
    $code = $childExit
    if ($null -eq $code) { try { $code = $p.ExitCode } catch { $code = $null } }
    if ($null -eq $code) { $code = if ((Test-Path (Join-Path $Root "app\out\build\win-amd64-relwithdebinfo\bioshock2.exe"))) { 0 } else { 1 } }
    exit $code
}
Set-Location $Root
$t0 = Get-Date
$InstallLog = Join-Path $Root ("logs\install_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
New-Item -ItemType Directory (Split-Path $InstallLog) -Force | Out-Null
try { Start-Transcript -Path $InstallLog -Force | Out-Null } catch {}
# Any uncaught error (setup.ps1, cmake, git) stops here with a message instead of closing the window.
trap { Die "Unexpected error: $($_.Exception.Message)`n  Full log: $InstallLog`n  Re-run Install.ps1 after fixing it: finished steps are skipped." }

Step "1. Build tools" 0.03
if ($SkipPrereqs) { Ok "skipped (-SkipPrereqs)" }
else {
    if ($Launcher -and -not $AcceptVsLicense -and -not (Test-VsCpp)) {
        Die "This PC has no Visual Studio C++ tools. Tick the Visual Studio licence in the setup options so it can be installed." 3
    }
    & (Join-Path $Root "tools\bootstrap.ps1") -SkipGhidra -SkipXenia -SkipDiscs -SkipSetup -MinFreeGB 60
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}

Step "2. Your game files" 0.25
if (Test-Path $xex) { Ok "game_bs2\ already extracted" }
else {
    if (-not $Iso) {
        if ($Launcher) { Die "No disc image: pick your BioShock 2 (Xbox 360) .iso in RecompLauncher." 2 }
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = "Select YOUR BioShock 2 (Xbox 360) disc image"
        $dlg.Filter = "Xbox 360 disc image (*.iso)|*.iso"
        if ($dlg.ShowDialog() -ne "OK") { Die "No disc image selected." 2 }
        $Iso = $dlg.FileName
    }
    if (-not (Test-Path $Iso)) { Die "Not found: $Iso" 2 }
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
    Report 0.28 "Extracting your disc"
    & (Join-Path $Root "tools\inspect_disc.ps1") -Iso $Iso -Name bs2 -ExtractXiso $xiso.FullName
    if (-not (Test-Path $xex)) { Die "Extraction finished but game_bs2\Bioshock2_Xenon.xex is missing. Is this the BioShock 2 (Xbox 360) disc?" 5 }
}

Step "3. Check it's the supported build" 0.35
$h = (Get-FileHash $xex -Algorithm SHA1).Hash.ToLower()
if ($h -ne $ExpectedXexSha1) {
    Die "Bioshock2_Xenon.xex SHA-1 is $h`n  Supported: $ExpectedXexSha1 (BioShock 2 360, TitleID 54540861).`n  Every address in this project is tied to that exact file. Other regions/versions are not supported yet." 5
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

Step "4. Build (SDK + code generation + compile; 20-60 min the first time)" 0.38
. (Join-Path $Root "tools\vsenv.ps1")
Report 0.40 "Building the patched ReXGlue SDK"
& (Join-Path $Root "tools\setup.ps1")
# Pin this repo's SDK: find_package would otherwise take whichever rexglue the CMake package registry lists first.
$RexCfg = Join-Path $Root ".deps\rexglue-sdk\out\install\win-amd64\lib\cmake\rexglue"
Push-Location (Join-Path $Root "app")
try {
    function Run([string[]]$a) { & cmake @a; if ($LASTEXITCODE -ne 0) { throw "cmake $($a -join ' ') failed" } }
    Report 0.50 "Configuring"
    Run @("--preset", "win-amd64-relwithdebinfo", "-Drexglue_DIR=$RexCfg")
    Report 0.55 "Generating C++ from your disc"
    Run @("--build", "--preset", "win-amd64-relwithdebinfo", "--target", "bioshock2_codegen")
    Run @("--preset", "win-amd64-relwithdebinfo", "-Drexglue_DIR=$RexCfg")   # re-configure: generated sources list only exists after codegen
    Report 0.65 "Compiling the game (the long part)"
    Run @("--build", "--preset", "win-amd64-relwithdebinfo")
} catch { Pop-Location; Die "Build failed: $($_.Exception.Message)`n  Full log: $InstallLog" 4 }
Pop-Location
$exe = Join-Path $Root "app\out\build\win-amd64-relwithdebinfo\bioshock2.exe"
if (-not (Test-Path $exe)) { Die "bioshock2.exe was not produced.`n  Full log: $InstallLog" 4 }
Ok "bioshock2.exe built"

Step "5. Offline multiplayer (private matches vs AI bots; 10-30 min)" 0.85
if (-not $mpOk) { Ok "skipped" }
else {
    try { & (Join-Path $Root "mp\tools\mp_init.ps1") -NoImport -RexBin (Join-Path $Root ".deps\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe") }
    catch { Write-Host "  [warn] multiplayer build failed: $($_.Exception.Message)`n  Single player is fine. Retry later: mp\tools\mp_init.ps1 -NoImport" -ForegroundColor Yellow }
    if (Test-Path (Join-Path $Root "mp\app\out\build\win-amd64-relwithdebinfo\bioshock2mp.exe")) { Ok "bioshock2mp.exe built" }
}

Step "6. Launcher shortcut" 0.98
if ($NoShortcut -or $Launcher) { Ok "skipped" }
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

$mins = ((Get-Date) - $t0).TotalMinutes
Write-Host ("`nDone in {0:N0} min. Start the game with the 'BioShock 2 Recomp' desktop shortcut (or BioShock2.cmd)." -f $mins) -ForegroundColor Green
if ($Launcher) {
    Report 1 ("Built in {0:N0} min" -f $mins)
    if ($ProgressFile) { Add-Content -Path $ProgressFile -Value "exit 0" -Encoding ascii }
    try { Stop-Transcript | Out-Null } catch {}
    exit 0
}
Read-Host "Press Enter to close"
