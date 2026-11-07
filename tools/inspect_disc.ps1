# Extract a 360 disc ISO and report what's on it: boot XEX identity, content packages, hashes.
#   .\tools\inspect_disc.ps1 -Iso ..\Disc1.iso -Name base1
# Output folder: .\game_<Name>\   Report: .\docs\discs\<Name>.txt
# Requires extract-xiso (path below). Windows PowerShell 5.1 compatible.
param(
    [Parameter(Mandatory)][string]$Iso,
    [Parameter(Mandatory)][string]$Name,
    [string]$ExtractXiso = "E:\Tools\artifacts\extract-xiso.exe",
    [switch]$SkipExtract
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Out  = Join-Path $Root "game_$Name"
$RepDir = Join-Path $Root "docs\discs"; New-Item -ItemType Directory $RepDir -Force | Out-Null
$Rep  = Join-Path $RepDir "$Name.txt"

function BE32([byte[]]$b, [int]$o) {
    [uint32](([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o+1] -shl 16) -bor ([uint32]$b[$o+2] -shl 8) -bor $b[$o+3])
}
function Read-Head([string]$path, [int]$len) {
    $fs = [IO.File]::OpenRead($path)
    try { $buf = New-Object byte[] $len; $n = $fs.Read($buf, 0, $len); if ($n -lt $len) { [Array]::Resize([ref]$buf, $n) }; ,$buf }
    finally { $fs.Close() }
}
function Get-XexInfo([string]$path) {
    $x = Read-Head $path 0x20000
    if ([Text.Encoding]::ASCII.GetString($x, 0, 4) -ne "XEX2") { return $null }
    $info = [ordered]@{ TitleID = ""; Module = ""; Disc = "" }
    $n = BE32 $x 0x14
    for ($i = 0; $i -lt [Math]::Min($n, 64); $i++) {
        $k = BE32 $x (0x18 + $i * 8); $v = [int64](BE32 $x (0x1C + $i * 8))   # may be an inline value (e.g. entry point), not an offset
        if ($k -eq 0x00040006 -and $v + 20 -lt $x.Length) {
            $info.TitleID = "{0:X8}" -f (BE32 $x ($v + 12)); $info.Disc = "{0}/{1}" -f $x[[int]$v + 18], $x[[int]$v + 19]
        }
        if ($k -eq 0x000183FF -and $v + 4 -lt $x.Length) {
            $len = [int64](BE32 $x $v)
            if ($len -gt 4 -and $v + $len -le $x.Length) { $info.Module = [Text.Encoding]::ASCII.GetString($x, [int]$v + 4, [int]$len - 4).Trim([char]0) }
        }
    }
    [pscustomobject]$info
}
function Get-PkgInfo([string]$path) {
    $h = Read-Head $path 0x1800
    if ($h.Length -lt 0x1700) { return $null }
    $magic = [Text.Encoding]::ASCII.GetString($h, 0, 4)
    if ($magic -notin @("CON ", "LIVE", "PIRS")) { return $null }
    [pscustomobject]@{
        Magic       = $magic.Trim()
        ContentType = "{0:X8}" -f (BE32 $h 0x344)
        TitleID     = "{0:X8}" -f (BE32 $h 0x360)
        DisplayName = [Text.Encoding]::BigEndianUnicode.GetString($h, 0x411, 0x80).Trim([char]0)
        TitleName   = [Text.Encoding]::BigEndianUnicode.GetString($h, 0x1691, 0x80).Trim([char]0)
    }
}

$log = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { Write-Host $s; $log.Add($s) }

Say "== Disc: $Name"
Say ("ISO: {0}  ({1:N0} bytes)" -f (Resolve-Path $Iso), (Get-Item $Iso).Length)
Say ("ISO SHA-1: " + (Get-FileHash $Iso -Algorithm SHA1).Hash)

if (-not $SkipExtract) {
    if (Test-Path $Out) { throw "$Out already exists. Delete it or use -SkipExtract." }
    if (-not (Test-Path $ExtractXiso)) { throw "extract-xiso not found: $ExtractXiso" }
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $ExtractXiso -d $Out $Iso 2>&1 | Out-Null; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    if (-not (Test-Path (Join-Path $Out "default.xex"))) { throw "extract-xiso produced no default.xex (exit $code)" }
}

$files = Get-ChildItem $Out -Recurse -File
Say ("Files: {0}   Total: {1:N0} bytes" -f $files.Count, ($files | Measure-Object Length -Sum).Sum)
Say ""
Say "-- Top level"
Get-ChildItem $Out | ForEach-Object { Say ("  {0,-30} {1,15:N0}" -f $_.Name, $(if ($_.PSIsContainer) { 0 } else { $_.Length })) }

Say ""
Say "-- XEX executables"
foreach ($f in $files | Where-Object { $_.Extension -in ".xex", ".xexp" }) {
    $xi = Get-XexInfo $f.FullName
    $sha = (Get-FileHash $f.FullName -Algorithm SHA1).Hash
    $rel = $f.FullName.Substring($Root.Length + 1)
    if ($xi) { Say ("  {0}`n    TitleID={1}  Module={2}  Disc={3}  SHA1={4}" -f $rel, $xi.TitleID, $xi.Module, $xi.Disc, $sha) }
    else     { Say ("  {0}  (not XEX2)  SHA1={1}" -f $rel, $sha) }
}

Say ""
Say "-- Content packages (CON/LIVE/PIRS)"
foreach ($f in $files | Where-Object { $_.Length -gt 0x2000 -and $_.Extension -eq "" }) {
    $pi = Get-PkgInfo $f.FullName
    if ($pi) {
        Say ("  {0}  ({1:N0} bytes)" -f $f.FullName.Substring($Root.Length + 1), $f.Length)
        Say ("    {0}  ContentType={1}  TitleID={2}  Title='{3}'  Display='{4}'" -f $pi.Magic, $pi.ContentType, $pi.TitleID, $pi.TitleName, $pi.DisplayName)
    }
}

Say ""
Say "-- Verdict"
$boot = Join-Path $Out "default.xex"
if (Test-Path $boot) {
    $bi = Get-XexInfo $boot
    if ($bi.Module -match "ExpansionInstaller") { Say "  Add-on/DLC installer disc. Game content is inside the package(s) above; extract with Velocity." }
    else { Say ("  Game disc. Boot XEX: TitleID={0} Module={1}. This is a recomp target." -f $bi.TitleID, $bi.Module) }
} else { Say "  No default.xex at root." }

$log | Set-Content -Encoding UTF8 $Rep
Write-Host "`nReport saved: $Rep"
