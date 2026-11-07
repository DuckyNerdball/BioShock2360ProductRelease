#Requires -Version 5.1
<#
One-shot environment setup for the BioShock 360 recomp. Safe to re-run: every step skips what's already done.

  Run from an ELEVATED Windows PowerShell (Visual Studio install needs admin):
    powershell -ExecutionPolicy Bypass -File .\tools\bootstrap.ps1

  Steps:
    1. winget packages: Git, CMake, Ninja, Python 3.12, Temurin JDK 21 (for Ghidra)
    2. Visual Studio 2022 Community + C++ workload + Clang/clang-cl components
    3. Clang >= 20 check (installs LLVM via winget if VS's clang is older)
    4. Xenia Canary, Ghidra + XEXLoaderWV into -ToolsDir
    5. Disk-space check
    6. Inspect every *.iso in -IsoDir (tools\inspect_disc.ps1), skipping ones already done
    7. Enter the VS x64 dev environment and run tools\setup.ps1 (SDK build + app scaffold)

  Manual steps that can't be scripted: extracting content packages in Velocity (GUI).
#>
param(
    [string]$ToolsDir = "E:\Tools",
    [string]$IsoDir   = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [int]$MinFreeGB   = 50,
    [switch]$SkipVS, [switch]$SkipGhidra, [switch]$SkipXenia, [switch]$SkipDiscs, [switch]$SkipSetup
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"   # PS 5.1 Invoke-WebRequest is very slow with the progress bar
$Root = Split-Path -Parent $PSScriptRoot

function Step($m) { Write-Host "`n=== $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  [ok]   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Info($m) { Write-Host "  $m" }

function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Update-SessionPath {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}
# Run a native command with EAP=Continue (PS 5.1 can turn native stderr into terminating errors). Returns output; exit code in $script:NativeExit.
function Invoke-Native([string]$Exe, [string[]]$CmdArgs) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { $out = & $Exe @CmdArgs 2>&1 | ForEach-Object { "$_" }; $script:NativeExit = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    $out
}
function Test-RealCommand([string]$Exe) {
    # Ignore the Microsoft Store app-execution-alias stubs (WindowsApps\python.exe etc.)
    $c = Get-Command $Exe -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch "\\WindowsApps\\" } | Select-Object -First 1
    [bool]$c
}
function Install-Winget([string]$Id, [string]$Exe) {
    if ($Exe -and (Test-RealCommand $Exe)) { Ok "$Id ($Exe on PATH)"; return }
    $listed = Invoke-Native winget @("list", "--id", $Id, "-e", "--accept-source-agreements") | Select-String -SimpleMatch $Id
    if ($listed) { Ok "$Id (installed)"; return }
    Info "Installing $Id ..."
    Invoke-Native winget @("install", "--id", $Id, "-e", "--silent", "--accept-package-agreements", "--accept-source-agreements") | Out-Host
    if ($script:NativeExit -ne 0) { Warn "winget install $Id returned $($script:NativeExit) (may still be fine; verifying below)" } else { Ok "$Id installed" }
}
function Get-GhLatest([string]$Repo) {
    Invoke-RestMethod -Headers @{ "User-Agent" = "bioshock-recomp-bootstrap" } "https://api.github.com/repos/$Repo/releases/latest"
}
function Get-GhTag([string]$Repo, [string]$Tag) {
    Invoke-RestMethod -Headers @{ "User-Agent" = "bioshock-recomp-bootstrap" } "https://api.github.com/repos/$Repo/releases/tags/$Tag"
}
function Save-And-Expand([string]$Url, [string]$Dest) {
    $zip = Join-Path $env:TEMP ([IO.Path]::GetFileName($Url))
    Info "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    New-Item -ItemType Directory $Dest -Force | Out-Null
    Expand-Archive $zip -DestinationPath $Dest -Force
    Remove-Item $zip -Force
}
function Get-VsPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $p = Invoke-Native $vswhere @("-version", "[17.0,18.0)", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath") | Select-Object -First 1
    if ($p) { return $p } else { return $null }
}
function Get-ClangMajor([string]$ClangExe) {
    $v = Invoke-Native $ClangExe @("--version") | Select-Object -First 1
    if ($v -match "version (\d+)\.") { return [int]$Matches[1] } else { return 0 }
}

# ---------------------------------------------------------------------------
Step "0. Preconditions"
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw "winget not found. Install 'App Installer' from the Microsoft Store, then re-run." }
Ok "winget present"
$isAdmin = Test-Admin
if ($isAdmin) { Ok "running elevated" } else { Warn "not elevated: Visual Studio install/modify will be skipped. Re-run as Administrator if VS is missing." }
New-Item -ItemType Directory $ToolsDir -Force | Out-Null

# ---------------------------------------------------------------------------
Step "1. Command-line tools (winget)"
Install-Winget "Git.Git"                         "git"
Install-Winget "Kitware.CMake"                   "cmake"
Install-Winget "Ninja-build.Ninja"               "ninja"
Install-Winget "Python.Python.3.12"              "python"
if (-not $SkipGhidra) { Install-Winget "EclipseAdoptium.Temurin.21.JDK" "java" }
Update-SessionPath

# ---------------------------------------------------------------------------
Step "2. Visual Studio 2022 + C++ + Clang"
$vsComponents = @(
    "Microsoft.VisualStudio.Workload.NativeDesktop",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
    "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
    "Microsoft.VisualStudio.Component.Windows11SDK.22621"
)
$vsPath = Get-VsPath
if ($SkipVS) { Warn "skipped (-SkipVS)" }
elseif (-not $vsPath) {
    if (-not $isAdmin) { Warn "VS 2022 not found and not elevated. Re-run as Administrator." }
    else {
        $override = ($vsComponents | ForEach-Object { "--add $_" }) -join " "
        $override += " --includeRecommended --passive --wait --norestart"
        Info "Installing Visual Studio 2022 Community (20-40 min, ~10 GB) ..."
        Invoke-Native winget @("install", "--id", "Microsoft.VisualStudio.2022.Community", "-e", "--accept-package-agreements", "--accept-source-agreements", "--override", $override) | Out-Host
        $vsPath = Get-VsPath   # trust vswhere, not the exit code (3010/reboot-required is common)
        if ($vsPath) { Ok "Visual Studio installed at $vsPath" } else { throw "Visual Studio install did not complete (winget exit $($script:NativeExit)). Reboot and re-run." }
    }
} else {
    Ok "Visual Studio 2022 at $vsPath"
    $clangVs = Join-Path $vsPath "VC\Tools\Llvm\x64\bin\clang.exe"
    if (-not (Test-Path $clangVs)) {
        if ($isAdmin) {
            Info "Adding Clang components to existing VS install ..."
            $installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vs_installer.exe"
            # BuildTools uses the VCTools workload; Community/Pro/Enterprise use NativeDesktop. vs_installer has no --wait.
            $prod = (Invoke-Native (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") @("-path", $vsPath, "-property", "productId") | Select-Object -First 1)
            $mod = $vsComponents
            if ($prod -match "BuildTools") { $mod = @("Microsoft.VisualStudio.Workload.VCTools") + ($vsComponents | Where-Object { $_ -ne "Microsoft.VisualStudio.Workload.NativeDesktop" }) }
            Info "Product: $prod"
            $vsArgs = @("modify", "--installPath", "`"$vsPath`"") + ($mod | ForEach-Object { @("--add", $_) }) + @("--passive", "--norestart")
            $p = Start-Process $installer -ArgumentList $vsArgs -Wait -PassThru
            if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "VS modify failed ($($p.ExitCode))" }
            Ok "Clang components added"
        } else { Warn "VS is missing the Clang components. Re-run elevated to add them." }
    } else { Ok "VS Clang present" }
}

# ---------------------------------------------------------------------------
Step "3. Clang >= 20 (ReXGlue presets call clang/clang++)"
$clangDir = $null
if ($vsPath) {
    $c = Join-Path $vsPath "VC\Tools\Llvm\x64\bin\clang.exe"
    if (Test-Path $c) {
        $maj = Get-ClangMajor $c
        if ($maj -ge 20) { $clangDir = Split-Path $c; Ok "VS clang $maj" } else { Warn "VS clang is $maj (< 20); using standalone LLVM instead" }
    }
}
if (-not $clangDir) {
    $c = Join-Path $env:ProgramFiles "LLVM\bin\clang.exe"
    if (-not (Test-Path $c)) { Install-Winget "LLVM.LLVM" $null }
    elseif ((Get-ClangMajor $c) -lt 20) { Info "Upgrading LLVM ..."; Invoke-Native winget @("upgrade", "--id", "LLVM.LLVM", "-e", "--silent", "--accept-package-agreements", "--accept-source-agreements") | Out-Host }
    if (Test-Path $c) {
        $maj = Get-ClangMajor $c
        if ($maj -ge 20) { $clangDir = Split-Path $c; Ok "LLVM clang $maj at $clangDir" }
        else { Warn "LLVM clang is $maj (< 20). Install LLVM 20+ from https://github.com/llvm/llvm-project/releases" }
    } else { Warn "clang not found at $c" }
}

# ---------------------------------------------------------------------------
Step "4a. Xenia Canary"
$xeniaDir = Join-Path $ToolsDir "xenia-canary"
if ($SkipXenia) { Warn "skipped (-SkipXenia)" }
elseif (Get-ChildItem $xeniaDir -Filter "xenia*.exe" -Recurse -ErrorAction SilentlyContinue) { Ok "already in $xeniaDir" }
else {
    try {
        $rel = Get-GhLatest "xenia-canary/xenia-canary-releases"
        $asset = $rel.assets | Where-Object { $_.name -match "windows" -and $_.name -match "\.zip$" } | Select-Object -First 1
        if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -match "\.zip$" } | Select-Object -First 1 }
        if (-not $asset) { throw "no zip asset in release $($rel.tag_name)" }
        Save-And-Expand $asset.browser_download_url $xeniaDir
        Ok "Xenia Canary $($rel.tag_name) -> $xeniaDir"
    } catch { Warn "Xenia download failed: $($_.Exception.Message). Get it manually: https://github.com/xenia-canary/xenia-canary-releases/releases" }
}

# ---------------------------------------------------------------------------
Step "4b. Ghidra + XEXLoaderWV"
if ($SkipGhidra) { Warn "skipped (-SkipGhidra)" }
else {
    $ghidraRoot = Join-Path $ToolsDir "ghidra"
    $existing = Get-ChildItem $ghidraRoot -Directory -Filter "ghidra_*_PUBLIC" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { Ok "Ghidra already at $($existing.FullName)"; $ghidraHome = $existing.FullName }
    else {
        try {
            # Pick the Ghidra version XEXLoaderWV's latest release was built for, so the plugin loads.
            $ldr = Get-GhLatest "zeroKilo/XEXLoaderWV"
            $ldrAsset = $ldr.assets | Where-Object { $_.name -match "\.zip$" } | Select-Object -First 1
            $gver = $null
            if ($ldrAsset -and $ldrAsset.name -match "ghidra_(\d+\.\d+(\.\d+)?)") { $gver = $Matches[1] }
            $grel = $null
            if ($gver) { try { $grel = Get-GhTag "NationalSecurityAgency/ghidra" "Ghidra_$($gver)_build" } catch { $grel = $null } }
            if (-not $grel) { Warn "couldn't match Ghidra to XEXLoaderWV ($($ldrAsset.name)); using latest Ghidra"; $grel = Get-GhLatest "NationalSecurityAgency/ghidra" }
            $gAsset = $grel.assets | Where-Object { $_.name -match "^ghidra_.*_PUBLIC_.*\.zip$" } | Select-Object -First 1
            Save-And-Expand $gAsset.browser_download_url $ghidraRoot
            $ghidraHome = (Get-ChildItem $ghidraRoot -Directory -Filter "ghidra_*_PUBLIC" | Select-Object -First 1).FullName
            Ok "Ghidra $($grel.tag_name) -> $ghidraHome"
            if ($ldrAsset) {
                $extDir = Join-Path $ghidraHome "Ghidra\Extensions"
                Save-And-Expand $ldrAsset.browser_download_url $extDir
                Ok "XEXLoaderWV $($ldr.tag_name) -> $extDir  (confirm in Ghidra: File > Install Extensions, tick XEXLoaderWV, restart)"
            }
        } catch { Warn "Ghidra setup failed: $($_.Exception.Message). Manual: https://github.com/NationalSecurityAgency/ghidra/releases + https://github.com/zeroKilo/XEXLoaderWV/releases" }
    }
}

# ---------------------------------------------------------------------------
Step "4c. Disc tools already in use"
foreach ($t in @("artifacts\extract-xiso.exe", "xdvdfs*\xdvdfs.exe", "Velocity*\Velocity.exe")) {
    $hit = Get-ChildItem (Join-Path $ToolsDir $t) -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { Ok $hit.FullName } else { Warn "not found under $ToolsDir : $t" }
}

# ---------------------------------------------------------------------------
Step "5. Disk space"
$drive = (Get-Item $Root).PSDrive
$freeGB = [math]::Round($drive.Free / 1GB, 1)
if ($freeGB -ge $MinFreeGB) { Ok "$($drive.Name): $freeGB GB free" } else { Warn "$($drive.Name): only $freeGB GB free (want $MinFreeGB+)" }

# ---------------------------------------------------------------------------
Step "6. Inspect disc images in $IsoDir"
if ($SkipDiscs) { Warn "skipped (-SkipDiscs)" }
else {
    $isos = Get-ChildItem $IsoDir -Filter *.iso -File -ErrorAction SilentlyContinue
    if (-not $isos) { Warn "no .iso files in $IsoDir" }
    foreach ($iso in $isos) {
        $name = ($iso.BaseName -replace "[^A-Za-z0-9_-]", "").ToLower()
        if (Test-Path (Join-Path $Root "game_$name")) { Ok "$($iso.Name) already extracted -> game_$name"; continue }
        Info "Inspecting $($iso.Name) as '$name' ..."
        try { & (Join-Path $PSScriptRoot "inspect_disc.ps1") -Iso $iso.FullName -Name $name -ExtractXiso (Join-Path $ToolsDir "artifacts\extract-xiso.exe") }
        catch { Warn "inspect failed for $($iso.Name): $($_.Exception.Message)" }
    }
    Info "Reports: $(Join-Path $Root 'docs\discs')"
}

# ---------------------------------------------------------------------------
Step "7. VS dev environment + tools\setup.ps1"
if ($SkipSetup) { Warn "skipped (-SkipSetup)" }
elseif (-not $vsPath) { Warn "no Visual Studio; skipping setup.ps1" }
elseif (-not $clangDir) { Warn "no clang >= 20; skipping setup.ps1" }
else {
    Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    $env:Path = "$clangDir;$env:Path"   # make sure the >=20 clang wins over anything VS put on PATH
    Ok ("clang: " + (Invoke-Native clang @("--version") | Select-Object -First 1))
    try { & (Join-Path $PSScriptRoot "setup.ps1") } catch { Warn "setup.ps1 failed: $($_.Exception.Message)" }
}

Step "Done"
Info "Manual next steps:"
Info "  - Velocity: extract any content package (PIRS/CON) into bioshock-recomp\game_<disc>_pkg\"
Info "  - Paste docs\discs\*.txt reports and any warnings above into the chat"
