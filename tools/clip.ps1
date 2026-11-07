# Copy the useful lines of the newest play log to the clipboard, ready to paste into chat.
#   .\tools\clip.ps1              bridge/give/console/verify lines from the newest play_*.log (last 150)
#   .\tools\clip.ps1 -All         the whole newest log
#   .\tools\clip.ps1 -Grep xyz    lines matching xyz (regex), last 150
#   .\tools\clip.ps1 -Build       re-run the build and copy its output instead
param([switch]$All, [string]$Grep, [switch]$Build, [int]$Last = 150)
$Root = Split-Path -Parent $PSScriptRoot
if ($Build) {
    . (Join-Path $Root "tools\vsenv.ps1")
    Push-Location (Join-Path $Root "app")
    try { $out = cmake --build --preset win-amd64-relwithdebinfo 2>&1 | Out-String } finally { Pop-Location }
    $out | Set-Clipboard; Write-Host "build output copied ($($out.Length) chars)"; return
}
$log = Get-ChildItem (Join-Path $Root "logs\play_*.log") | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $log) { throw "no play_*.log" }
$lines = Get-Content $log.FullName
if (-not $All) {
    $pat = if ($Grep) { $Grep } else { 'bioshock2: (give|ran |all weapons|names)|verify:|bridge: (ProcessEvent|top events)|StaticLoadObject|BS1Game|Package|version|crash|exception|Title terminated' }
    $lines = $lines | Where-Object { $_ -match $pat } | Select-Object -Last $Last
}
$text = "# $($log.Name)`n" + ($lines -join "`n")
$text | Set-Clipboard
Write-Host "$($log.Name): $($lines.Count) lines copied to clipboard"
