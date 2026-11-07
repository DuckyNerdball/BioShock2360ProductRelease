# Summarise a run/play log: crashes, GPU errors (texture/shader), hitches, guest fps, user marks (F10).
#   .\tools\report.ps1                 # newest log
#   .\tools\report.ps1 logs\run_X.log
param([string]$Log)
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Log) { $Log = (Get-ChildItem (Join-Path $Root "logs") -Include run_*.log, play_*.log -Recurse | Sort-Object LastWriteTime | Select-Object -Last 1).FullName }
if (-not (Test-Path $Log)) { Write-Host "No log found." -ForegroundColor Red; return }
$lines = Get-Content $Log
function Strip([string]$l) { $l -replace '^\[[^\]]*\]\s*', '' -replace '\[t\d+\]\s*', '' }
function Section([string]$title, $items, [string]$color = 'Yellow') {
    if ($items -and @($items).Count) { Write-Host "`n$title ($(@($items).Count))" -ForegroundColor $color; @($items) | Select-Object -First 15 | ForEach-Object { "  $_" } }
    else { Write-Host "`n$title : none" -ForegroundColor Green }
}
Write-Host "Report: $Log" -ForegroundColor Cyan

# Crash / fatal
$crash = $lines | Where-Object { $_ -match 'unregistered function|\[critical\]|Unhandled|access violation|EXCEPTION|abort|assert' }
Section "CRASH / FATAL" ($crash | ForEach-Object { Strip $_ }) 'Red'

# GPU problems that show up as broken textures/geometry
$gpu = $lines | Where-Object { $_ -match '\[(gpu|graphics)\]' -and $_ -match 'Unsupported texture|translation failed|Failed to translate|Failed to create .*pipeline|resolve.*(fail|clamp|invalid)|\[error\]' }
Section "GPU ERRORS (texture/shader/resolve)" ($gpu | ForEach-Object { Strip $_ } | Group-Object | Sort-Object Count -Descending | ForEach-Object { "{0,5}x {1}" -f $_.Count, $_.Name }) 'Red'

# Other warnings/errors, minus known harmless noise
$noise = 'NtCreateFile\] FAILED|IoDismountVolumeByFileHandle|GameControllerDB|ShaderDumpxe|composite c\d|sharp composite|depth resolve pitch override|diag\d? |USER MARK|guest fps|HITCH |set deleting flag '
$other = $lines | Where-Object { $_ -match '\[(warning|error)\]' -and $_ -notmatch $noise -and $gpu -notcontains $_ }
Section "OTHER WARNINGS/ERRORS" ($other | ForEach-Object { (Strip $_) -replace '0x[0-9A-Fa-f]+', '0x?' } | Group-Object | Sort-Object Count -Descending | ForEach-Object { "{0,5}x {1}" -f $_.Count, $_.Name })

# Hitches
$h = $lines | Where-Object { $_ -match 'HITCH ([\d.]+) ms \| new PSOs (\d+) \| texture loads (\d+) \(([\d.]+) ms\)' } | ForEach-Object {
    $null = $_ -match 'HITCH ([\d.]+) ms \| new PSOs (\d+) \| texture loads (\d+) \(([\d.]+) ms\)'
    [pscustomobject]@{ Time = ($_ -replace '^\[\S+ (\S+)\].*', '$1'); Ms = [double]$matches[1]; Pso = [int]$matches[2]; Tex = [int]$matches[3]; TexMs = [double]$matches[4] } }
if ($h) {
    $shader = @($h | Where-Object Pso -gt 0).Count; $tex = @($h | Where-Object { $_.Pso -eq 0 -and $_.TexMs -ge 10 }).Count
    $game = @($h).Count - $shader - $tex
    Write-Host "`nHITCHES ($(@($h).Count)): worst $(($h | Measure-Object Ms -Maximum).Maximum) ms | shader compile $shader | texture upload $tex | game-side/streaming $game" -ForegroundColor Yellow
    $h | Sort-Object Ms -Descending | Select-Object -First 8 | ForEach-Object { "  {0}  {1,6:N1} ms  PSOs {2}  tex {3} ({4} ms)" -f $_.Time, $_.Ms, $_.Pso, $_.Tex, $_.TexMs }
} else { Write-Host "`nHITCHES : none" -ForegroundColor Green }

# Guest fps
$fps = $lines | Where-Object { $_ -match 'guest fps ([\d.]+)' } | ForEach-Object { $null = $_ -match 'guest fps ([\d.]+)'; [double]$matches[1] }
if ($fps) { $s = $fps | Measure-Object -Average -Minimum -Maximum
    Write-Host ("`nGUEST FPS: avg {0:N1} | min {1:N1} | max {2:N1} | samples below 50: {3}" -f $s.Average, $s.Minimum, $s.Maximum, @($fps | Where-Object { $_ -lt 50 }).Count) -ForegroundColor Cyan }

# User marks (press F10 in-game when you see a texture glitch)
$marks = $lines | Where-Object { $_ -match 'USER MARK' } | ForEach-Object { $_ -replace '^\[\S+ (\S+)\].*', '$1' }
Section "USER MARKS (F10 = texture/visual glitch you flagged)" $marks 'Magenta'
