# Patch BioShock 2 MULTIPLAYER ini values inside game_bs2\multiplayer\ConfigINI.IBF (UTF-16LE text, same-length edits only).
# Local game data only (never committed). Backup: ConfigINI.IBF.orig next to it (made once).
#   .\tools\mp_ini.ps1                       # round countdown 10 -> 05 s (Hardcore 15 -> 05)
#   .\tools\mp_ini.ps1 -Round 3              # any 1-2 digit value
#   .\tools\mp_ini.ps1 -Restore              # put the original back
param([ValidateRange(1, 99)][int]$Round = 5, [switch]$Restore)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Ibf  = Join-Path $Root "game_bs2\multiplayer\ConfigINI.IBF"
$Orig = "$Ibf.orig"
if (-not (Test-Path $Ibf)) { throw "Not found: $Ibf" }
if (-not (Test-Path $Orig)) { Copy-Item $Ibf $Orig; Write-Host "Backup: $Orig" }
if ($Restore) { Copy-Item $Orig $Ibf -Force; Write-Host "Restored original ConfigINI.IBF"; return }
# Byte-level replace (UTF-16LE patterns). NEVER decode/re-encode the whole file: it has binary sections, and a
# GetString/GetBytes round trip corrupted 2 bytes (unpaired surrogate -> FFFD) on 11-07 -> the game exited at boot.
[byte[]]$b = [IO.File]::ReadAllBytes($Orig)
$enc = [Text.Encoding]::Unicode
$v = "{0:D2}" -f $Round
$pairs = @(@('RoundCountdownDuration=10', "RoundCountdownDuration=$v"),
           @('HardcoreRoundCountdownDuration=15', "HardcoreRoundCountdownDuration=$v"))
function Find-Bytes([byte[]]$hay, [byte[]]$needle) {
    for ($i = 0; $i -le $hay.Length - $needle.Length; $i += 2) {
        $ok = $true
        for ($j = 0; $j -lt $needle.Length; $j++) { if ($hay[$i + $j] -ne $needle[$j]) { $ok = $false; break } }
        if ($ok) { return $i }
    }
    return -1
}
foreach ($p in $pairs) {
    $o = $enc.GetBytes($p[0]); $n = $enc.GetBytes($p[1])
    $i = Find-Bytes $b $o
    if ($i -lt 0) { Write-Host "Key not found (skipped): $($p[0])" -ForegroundColor Yellow; continue }
    [Array]::Copy($n, 0, $b, $i, $n.Length); Write-Host ("{0} -> {1} at byte {2}" -f $p[0], $p[1], $i)
}
[IO.File]::WriteAllBytes($Ibf, $b)
Write-Host "Patched $Ibf"
