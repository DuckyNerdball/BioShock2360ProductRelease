# Launch the BioShock recomp.
#   .\tools\play.ps1                    # -Look Pure (default: zero blur)
#   .\tools\play.ps1 -Look Clean        # 3x supersampling, lighter FXAA, dither
#   .\tools\play.ps1 -Look Ultra        # 4x supersampling
#   .\tools\play.ps1 -Look Cinematic    # original DOF/bloom on
#   Any explicit flag overrides the look:  -Look Ultra -Scale 5 -Sharpness 0.4 -AA none -Dof -Delta -4
#   .\tools\play.ps1 -Dof -Bloom 0.25 -Exposure 1.5   # real bloom at 25% strength (needs SDK patch: bioshock_bloom_scale)
#   .\tools\play.ps1 -Fps30          # original 30 fps cap (default is 60 fps via 120 Hz guest vblank)
#   .\tools\play.ps1 -Extra '--some_cvar=1','--other=2'   # pass raw cvars through
#   .\tools\play.ps1 -RestoreAA         # put the original ConfigINI.IBF back and exit
param([ValidateSet('Pure','Crisp','Sharp','Clean','Ultra','Cinematic','BS2')][string]$Look, [ValidateRange(1,6)][int]$Scale, [int]$Delta, [ValidateRange(-1,5)][int]$Aniso = 5, [ValidateSet('bilinear','cas','fsr')][string]$Present = 'cas', [double]$Sharpness, [double]$Bloom, [double]$Exposure, [double]$Gamma, [switch]$Fps30, [switch]$Dof, [switch]$NoDof, [ValidateSet('none','fxaa','fxaa_extreme')][string]$AA, [switch]$NoDirectResolve, [switch]$SkipComposite, [switch]$RestoreAA, [string]$Preset = "win-amd64-relwithdebinfo", [string[]]$Extra = @(), [string]$Project = "bioshock", [string]$GameDir = "game_base1")
. (Join-Path $PSScriptRoot 'look.ps1')
if (-not $Look) { $Look = if ($Project -like "bioshock2*") { 'BS2' } else { 'Pure' } }  # per-game default look
$L = $Looks[$Look].Clone()
if ($PSBoundParameters.ContainsKey('Scale')) { $L.Scale = $Scale }
if ($PSBoundParameters.ContainsKey('Sharpness')) { $L.Sharpness = $Sharpness }
if ($PSBoundParameters.ContainsKey('AA')) { $L.AA = $AA }
if ($PSBoundParameters.ContainsKey('Bloom')) { $L.Bloom = $Bloom }
if ($PSBoundParameters.ContainsKey('Gamma')) { $L.Gamma = $Gamma }
if ($PSBoundParameters.ContainsKey('Exposure')) { $L.Exposure = $Exposure }
if ($PSBoundParameters.ContainsKey('Delta')) { $L.Delta = $Delta }
if ($Dof) { $L.Dof = $true }; if ($NoDof) { $L.Dof = $false }
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Ibf  = Join-Path $Root "$GameDir\ConfigINI.IBF"
$Orig = "$Ibf.orig"
$Exe  = Join-Path $Root "app\out\build\$Preset\$Project.exe"
$HasIbf = Test-Path $Ibf
if (-not $HasIbf) { Write-Host "No ConfigINI.IBF in $GameDir - skipping the AA/edge-blur ini patch" -ForegroundColor DarkGray }
if ($HasIbf -and -not (Test-Path $Orig)) { Copy-Item $Ibf $Orig; Write-Host "Backed up original to $Orig" }
if ($RestoreAA) { if ($HasIbf) { Copy-Item $Orig $Ibf -Force; Write-Host "Restored original ConfigINI.IBF" }; return }
if ($HasIbf) {

# Always patch from the pristine copy (idempotent). Same-length edits only: file size must not change.
$enc = [Text.Encoding]::GetEncoding(28591)
$s = $enc.GetString([IO.File]::ReadAllBytes($Orig))
$pairs = @(@('EdgeDetectCutoff=0.002', 'EdgeDetectCutoff=9.999'),
           @('EdgeBlurSize=0.5',       'EdgeBlurSize=0.0'),
           @('UseSoftwareAntiAliasing=True', 'UseSoftwareAntiAliasing=No  '))
foreach ($p in $pairs) {
    $n = ([regex]::Matches($s, [regex]::Escape($p[0]))).Count
    if ($n -lt 1) { throw "Key not found in original IBF: $($p[0])" }
    Write-Host ("{0} (x{1}) -> {2}" -f $p[0], $n, $p[1])
    $s = $s.Replace($p[0], $p[1])
}
$bytes = $enc.GetBytes($s)
if ($bytes.Length -ne (Get-Item $Orig).Length) { throw "Length changed - aborting" }
[IO.File]::WriteAllBytes($Ibf, $bytes)
}

New-Item -ItemType Directory (Join-Path $Root "logs"), (Join-Path $Root "run\userdata") -Force | Out-Null
$log = Join-Path $Root ("logs\play_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
Write-Host "Look $Look : scale $($L.Scale) ($(1280*$L.Scale)x$(720*$L.Scale)), AA $($L.AA), CAS $($L.Sharpness), bloom $(if ($L.Dof) { $L.Bloom } else { 'off' }), exposure $($L.Exposure), gamma $($L.Gamma), delta $($L.Delta), dither $($L.Dither). Log: $log"
Start-Process $Exe -Wait -WorkingDirectory $Root -ArgumentList (@(Get-LookArgs $L -Aniso $Aniso -Present $Present -DirectResolve (-not $NoDirectResolve) -SkipComposite ($SkipComposite.IsPresent -or [bool]$L.SkipComposite) -GuestHz $(if ($Fps30) { 60 } else { 120 })) + @(
    '--game_data_root', "`"$Root\$GameDir`"", '--user_data_root', "`"$Root\run\userdata`"",
    '--log_file', "`"$log`"") + $Extra)
