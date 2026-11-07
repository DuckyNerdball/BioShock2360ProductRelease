# Which game does this repo build? Derived from the single app\*_manifest.toml, so the same
# tools work unchanged in the BioShock 1 and BioShock 2 repos. Dot-source:
#   . (Join-Path $PSScriptRoot 'game.ps1')
# Sets $GameInfo = @{ Project; Dir; Root; Xex; Ibf; Exe(preset) }
#   Project  bioshock | bioshock2          (exe name, manifest prefix, codegen target prefix)
#   Dir      game_base1 | game_bs2          (folder under repo root, from game_root)
#   Xex      absolute path of the entrypoint XEX (from [entrypoint] file_path)
#   Ibf      BS1 only: ConfigINI.IBF for play.ps1's AA patch. $null for BS2 (its IBF is packed).
$__repo = Split-Path -Parent $PSScriptRoot
$__man  = @(Get-ChildItem (Join-Path $__repo 'app') -Filter '*_manifest.toml' -File -ErrorAction SilentlyContinue)
if ($__man.Count -ne 1) { throw "Expected exactly one app\*_manifest.toml, found $($__man.Count)." }
$__raw  = Get-Content $__man[0].FullName -Raw
$__gr   = [regex]::Match($__raw, '(?m)^\s*game_root\s*=\s*"([^"]+)"')
$__fp   = [regex]::Match($__raw, '(?m)^\s*file_path\s*=\s*"([^"]+)"')
if (-not $__gr.Success) { throw "game_root not found in $($__man[0].Name)" }
$GameInfo = @{}
$GameInfo.Project = $__man[0].BaseName -replace '_manifest$', ''
$GameInfo.Dir     = Split-Path -Leaf ($__gr.Groups[1].Value -replace '/', '\')
$GameInfo.Root    = Join-Path $__repo $GameInfo.Dir
$GameInfo.Xex     = $null
if ($__fp.Success) {
    $__x = $__fp.Groups[1].Value -replace '/', '\'
    if (-not [IO.Path]::IsPathRooted($__x)) { $__x = Join-Path (Join-Path $__repo 'app') $__x }
    $GameInfo.Xex = [IO.Path]::GetFullPath($__x)
}
$GameInfo.Ibf = $null
if ($GameInfo.Project -eq 'bioshock') { $GameInfo.Ibf = Join-Path $GameInfo.Root 'ConfigINI.IBF' }
$GameInfo.Exe = { param([string]$Preset = 'win-amd64-relwithdebinfo') Join-Path $__repo "app\out\build\$Preset\$($GameInfo.Project).exe" }
