# One-time SDK build + app scaffold on Windows x64.
# Run inside a VS 2022 x64 dev environment (tools\bootstrap.ps1 does this for you), from the repo root:
#   .\tools\setup.ps1
# Needs on PATH: git, cmake (3.25+), ninja, python, clang/clang++ 20+ (the SDK presets use clang, not clang-cl).
$ErrorActionPreference = "Stop"
$RexTag  = "v0.10.0"                 # pinned SDK release; bump deliberately and note it in docs/STATUS.md
$Root    = Split-Path -Parent $PSScriptRoot
$SdkDir  = Join-Path $Root ".deps\rexglue-sdk"
$AppDir  = Join-Path $Root "app"

# Run a native command; fail on non-zero exit. EAP=Continue so stderr chatter (git progress) isn't fatal in PS 5.1.
function Invoke-Native([string]$Exe, [string[]]$CmdArgs) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $Exe @CmdArgs; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "$Exe $($CmdArgs -join ' ') failed (exit $code)" }
}

foreach ($tool in "git", "cmake", "ninja", "clang", "clang++", "python") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing on PATH: $tool" }
}
$clangLine = (& clang --version 2>$null | Select-Object -First 1)
if ($clangLine -notmatch "version (\d+)\." -or [int]$Matches[1] -lt 20) { throw "clang 20+ required, found: $clangLine" }
if (-not $env:VCToolsInstallDir) { throw "Not in a VS dev environment (VCToolsInstallDir unset). Use tools\bootstrap.ps1 or 'Developer PowerShell for VS 2022'." }
Write-Host "clang: $clangLine"

# Submodules (e.g. libmspack) contain git symlinks. On Windows they only check out correctly with
# core.symlinks=true AND symlink rights (elevated shell or Developer Mode); otherwise they become text files.
$probe = Join-Path $env:TEMP ("symlink_probe_" + [guid]::NewGuid())
try { New-Item -ItemType SymbolicLink -Path $probe -Target $env:TEMP -ErrorAction Stop | Out-Null; Remove-Item $probe -Force }
catch { throw "Can't create symlinks. Run elevated or enable Developer Mode (Settings > System > For developers)." }

# 1. SDK at the pinned tag
if (-not (Test-Path $SdkDir)) {
    Invoke-Native git @("-c", "core.symlinks=true", "clone", "--recursive", "--branch", $RexTag, "https://github.com/rexglue/rexglue-sdk.git", $SdkDir)
}
Push-Location $SdkDir
try {
    Invoke-Native git @("fetch", "--tags")
    # 2. Reset to the tag, then apply local SDK patches in order (idempotent)
    Invoke-Native git @("reset", "--hard", "--quiet", $RexTag)
    Invoke-Native git @("-c", "core.symlinks=true", "submodule", "update", "--init", "--recursive")
    Get-ChildItem (Join-Path $Root "patches\rexglue-sdk\*.patch") -ErrorAction SilentlyContinue |
        Sort-Object Name | ForEach-Object {
            Write-Host "Applying $($_.Name)"
            Invoke-Native git @("apply", "--whitespace=nowarn", $_.FullName)
        }

    # 3. Build + install (install prefix: out\install\win-amd64; registers find_package(rexglue))
    Invoke-Native cmake @("--preset", "win-amd64")
    Invoke-Native cmake @("--build", "out/build/win-amd64", "--config", "Release", "--target", "install")
} finally { Pop-Location }

$RexBin = Join-Path $SdkDir "out\install\win-amd64\bin\rexglue.exe"
if (-not (Test-Path $RexBin)) {
    $hit = Get-ChildItem -Path (Join-Path $SdkDir "out") -Recurse -Filter rexglue.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit) { throw "rexglue.exe not found under $SdkDir\out after install." }
    $RexBin = $hit.FullName
}
Write-Host "rexglue CLI: $RexBin"

# 4. Scaffold the app once
if (-not (Test-Path (Join-Path $AppDir "CMakeLists.txt"))) {
    # v0.10.0 CLI: --project-name / --xex-path (required) / --game-root / --project-root. Writes bioshock_manifest.toml.
    $Xex = Join-Path $Root "game_base1\default.xex"
    if (-not (Test-Path $Xex)) { throw "Base game XEX not found: $Xex (run tools\inspect_disc.ps1 -Name base1 first)" }
    Invoke-Native $RexBin @("init", "--project-name", "bioshock", "--xex-path", $Xex, "--game-root", (Split-Path $Xex), "--project-root", $AppDir)
}

# 5. Repo + hooks
if (-not (Test-Path (Join-Path $Root ".git"))) { Invoke-Native git @("-C", $Root, "init") }
Invoke-Native git @("-C", $Root, "config", "core.hooksPath", ".githooks")
Write-Host "Setup done."
