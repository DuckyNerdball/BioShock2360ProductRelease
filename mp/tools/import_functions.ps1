# Bulk-declare vtable targets the analyzer missed (instead of one fix_loop iteration per crash).
# Run from the repo root. -Build needs the VS dev shell (auto dot-sources tools\vsenv.ps1).
#   .\tools\import_functions.ps1 -DryRun            # extract PE if needed, self-test, show candidates
#   .\tools\import_functions.ps1 -Build             # append to manifest, codegen + configure + build
#   Project/XEX come from the manifest via tools\game.ps1; works as-is in the BS1 and BS2 repos.
# Needs: python + pefile + cryptography (pip install pefile cryptography). XEX is extracted with
# tools\xex_extract.py (basic compression); xextool.exe (PATH or .deps\xextool\) only as LZX fallback.
param(
    [string]$Project = "",   # default: tools\game.ps1
    [string]$Xex     = "",   # default: manifest [entrypoint] file_path
    [string]$Preset  = "win-amd64-relwithdebinfo",
    [int]$MaxNew     = 5000,
    [switch]$DryRun,
    [switch]$Build,
    [switch]$Force   # skip the self-test gate
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'game.ps1')
if (-not $Project) { $Project = $GameInfo.Project }
$Root     = Split-Path -Parent $PSScriptRoot
$App      = Join-Path $Root "app"
$Manifest = Join-Path $App "${Project}_manifest.toml"
$LogDir   = Join-Path $Root "logs"
$Dumps    = Join-Path $Root "dumps"
$stamp    = Get-Date -Format "yyyyMMdd_HHmmss"
New-Item -ItemType Directory $LogDir, $Dumps -Force | Out-Null
if (-not (Test-Path $Manifest)) { throw "Manifest not found: $Manifest" }
$raw = Get-Content $Manifest -Raw

# --- generated dir / funcs.h (everything the analyzer already knows)
$m = [regex]::Match($raw, 'out_directory_path\s*=\s*"([^"]+)"')
if (-not $m.Success) { throw "out_directory_path not found in $Manifest" }
$GenDir = Join-Path $App ($m.Groups[1].Value -replace '/', '\')
$FuncsH = Get-ChildItem $GenDir -Filter *_funcs.h -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $FuncsH) { throw "No *_funcs.h in $GenDir. Run codegen once first." }

# --- base PE
$XexPath = if ($Xex) { Join-Path $Root $Xex } else { $GameInfo.Xex }
if (-not (Test-Path $XexPath)) { throw "XEX not found: $XexPath" }
$BasePe = Join-Path $Dumps ("{0}_base.exe" -f [IO.Path]::GetFileNameWithoutExtension($XexPath))
# --- python
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { throw "python not on PATH" }
$py = $py.Source
$ErrorActionPreference = "Continue"
& $py -c "import pefile, cryptography" 2>$null
if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = "Stop"; throw "missing modules: python -m pip install pefile cryptography" }
$ErrorActionPreference = "Stop"
$Scan = Join-Path $PSScriptRoot "vtscan.py"

# --- base PE
if (-not (Test-Path $BasePe)) {
    Write-Host "Extracting base PE -> $BasePe"
    $ErrorActionPreference = "Continue"
    & $py (Join-Path $PSScriptRoot 'xex_extract.py') $XexPath $BasePe 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    $ErrorActionPreference = "Stop"
    if (-not (Test-Path $BasePe)) {
        $xt = $null
        $cmd = Get-Command xextool -ErrorAction SilentlyContinue
        if ($cmd) { $xt = $cmd.Source }
        if (-not $xt) { $c = Join-Path $Root ".deps\xextool\xextool.exe"; if (Test-Path $c) { $xt = $c } }
        if (-not $xt) { throw "xex_extract.py failed (see above) and no xextool fallback in .deps\xextool\ or PATH." }
        & $xt -b $BasePe $XexPath
        if (-not (Test-Path $BasePe)) { throw "xextool did not produce $BasePe" }
    }
}

# --- known set: funcs.h + manifest
$known = @{}
foreach ($mm in [regex]::Matches((Get-Content $FuncsH.FullName -Raw), '(?i)(?<![0-9A-F])8[23][0-9A-F]{6}(?![0-9A-F])')) { $known["0x" + $mm.Value.ToUpper()] = $true }
foreach ($mm in [regex]::Matches($raw, '"0x([0-9A-Fa-f]{8})"\s*=')) { $known["0x" + $mm.Groups[1].Value.ToUpper()] = $true }
$KnownFile = Join-Path $LogDir "known_$Project.txt"
$known.Keys | Sort-Object | Set-Content -Encoding ascii $KnownFile
Write-Host "Known functions: $($known.Count)"

function Invoke-Scan([string]$Out, [string]$Known) {
    $a = @($Scan, $BasePe); if ($Known) { $a += $Known }
    $ErrorActionPreference = "Continue"
    & $py @a 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host "  $_" -ForegroundColor DarkGray } else { $_ }
    } | Set-Content -Encoding ascii $Out
    $ErrorActionPreference = "Stop"
}

# --- self-test: does an unfiltered scan find the addresses fix_loop found at runtime? (BS2: 21/21)
$runtime = @([regex]::Matches($raw, 'found at runtime[^\r\n]*\r?\n"(0x[0-9A-Fa-f]{8})"') | ForEach-Object { "0x" + $_.Groups[1].Value.Substring(2).ToUpper() })
$AllFile = Join-Path $LogDir "vt_all_$Project.txt"
Invoke-Scan $AllFile $null
$all = @{}; Get-Content $AllFile | Where-Object { $_ -match '^0x' } | ForEach-Object { $all[$_] = $true }
if ($runtime.Count) {
    $hit = @($runtime | Where-Object { $all.ContainsKey($_) }).Count
    $pct = [math]::Round(100 * $hit / $runtime.Count)
    Write-Host "Self-test: scanner finds $hit / $($runtime.Count) runtime-found addresses ($pct%)" -ForegroundColor $(if ($pct -ge 60) { 'Green' } else { 'Red' })
    if ($pct -lt 60 -and -not $Force) { throw "Scanner hit rate too low to trust a bulk import. Inspect $AllFile (or rerun with -Force)." }
} else { Write-Host "Self-test skipped: no fix_loop entries in manifest yet." -ForegroundColor Yellow }

# --- new candidates
$Cands = Join-Path $LogDir "vt_new_$Project.txt"
Invoke-Scan $Cands $KnownFile
$new = @(Get-Content $Cands | Where-Object { $_ -match '^0x[0-9A-F]{8}$' -and -not $known.ContainsKey($_) })
Write-Host "New candidates: $($new.Count)  ($Cands)" -ForegroundColor Cyan
if ($new.Count -eq 0) { return }
if ($new.Count -gt $MaxNew) { throw "Too many candidates ($($new.Count) > $MaxNew): filter probably too loose. Inspect $Cands or raise -MaxNew." }
if ($DryRun) { $new | Select-Object -First 20; Write-Host "(dry run: manifest untouched)"; return }

# --- append (same rules as fix_loop: [entrypoint.functions] must be the last table)
$last = ([regex]::Matches($raw, '(?m)^\[[^\]]+\]') | Select-Object -Last 1).Value
if ($last -ne '[entrypoint.functions]') { throw "Manifest's last table is $last, not [entrypoint.functions]. Move it to the end first." }
$Bak = Join-Path $LogDir "manifest_${Project}_$stamp.toml.bak"
Copy-Item $Manifest $Bak
Add-Content $Manifest (@("# $(Get-Date -Format yyyy-MM-dd): vtable-scan bulk import ($($new.Count) entries, tools\import_functions.ps1)") + ($new | ForEach-Object { "`"$_`" = {}" }))
Add-Content (Join-Path $Root "docs\STATUS.md") "- $(Get-Date -Format yyyy-MM-dd) - vtable-scan bulk import: $($new.Count) functions declared in [entrypoint.functions] (import_functions)"
Write-Host "Appended $($new.Count). Backup: $Bak" -ForegroundColor Green
Write-Host "Roll back: Copy-Item '$Bak' '$Manifest' -Force"

if ($Build) {
    if (-not $env:VCToolsInstallDir) { . (Join-Path $PSScriptRoot 'vsenv.ps1') }
    $BuildLog = Join-Path $LogDir "build_import_$stamp.log"
    Push-Location $App
    try {
        foreach ($step in @(@("--build", "--preset", $Preset, "--target", "${Project}_codegen"),
                            @("--preset", $Preset),
                            @("--build", "--preset", $Preset))) {
            Write-Host "cmake $($step -join ' ')"
            cmd /c "cmake $($step -join ' ') 2>&1" | Out-File -Append -Encoding utf8 $BuildLog
            if ($LASTEXITCODE -ne 0) { throw "cmake $($step -join ' ') failed. See $BuildLog. Roll back: Copy-Item '$Bak' '$Manifest' -Force" }
        }
    } finally { Pop-Location }
    Write-Host "Build OK. Next: .\tools\fix_loop.ps1 -Project $Project -GameDir $($GameInfo.Dir) -RunSeconds 90" -ForegroundColor Green
}
