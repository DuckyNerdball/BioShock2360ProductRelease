# Enter the Visual Studio 2022 x64 developer environment in the CURRENT PowerShell session and put a
# clang >= 20 first on PATH. Dot-source it:   . .\tools\vsenv.ps1
# Used by Install.ps1, Launcher.ps1 (Build + Play) and anyone building outside a "Developer PowerShell".
$ErrorActionPreference = "Stop"
if ($env:VCToolsInstallDir -and $env:BS_CLANG_OK) { return }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio 2022 not found. Run Install.ps1 first." }
$vs = & $vswhere -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw "Visual Studio 2022 with the C++ tools not found. Run Install.ps1 first." }
if (-not $env:VCToolsInstallDir) {
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}
function Get-ClangMajor([string]$exe) { $v = & $exe --version 2>$null | Select-Object -First 1; if ($v -match "version (\d+)\.") { [int]$Matches[1] } else { 0 } }
$cands = @((Join-Path $vs "VC\Tools\Llvm\x64\bin\clang.exe"), (Join-Path $env:ProgramFiles "LLVM\bin\clang.exe"))
$clang = $cands | Where-Object { (Test-Path $_) -and (Get-ClangMajor $_) -ge 20 } | Select-Object -First 1
if (-not $clang) { throw "clang 20+ not found (VS or C:\Program Files\LLVM). Run Install.ps1 first." }
$env:Path = (Split-Path $clang) + ";" + $env:Path
$env:BS_CLANG_OK = "1"
Write-Host "VS dev environment ready (clang $(Get-ClangMajor $clang))." -ForegroundColor DarkGray
