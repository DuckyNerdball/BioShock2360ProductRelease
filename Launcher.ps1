#Requires -Version 5.1
# BioShock 2 360 Recomp launcher (Windows Forms; no extra installs). Open with BioShock2.cmd or a desktop shortcut.
# Settings are remembered in run\launcher.json. Every button just calls the scripts in tools\.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$Root = $PSScriptRoot
$Exe = Join-Path $Root "app\out\build\win-amd64-relwithdebinfo\bioshock2.exe"
$MpRoot = Join-Path $Root "mp"
$MpExe = Join-Path $MpRoot "app\out\build\win-amd64-relwithdebinfo\bioshock2mp.exe"
$MpIni = Join-Path $MpRoot "run\mp_bots.ini"
$Cfg = Join-Path $Root "run\launcher.json"
$Looks = @("BS2", "Sharp", "Clean", "Ultra")
$LookInfo = @{
    BS2 = "Default. BioShock 2 tuned: 2x, no blur, warm exposure 1.0 / gamma 1.05."
    Sharp = "2x, strongest anti-aliasing."
    Clean = "3x supersampling. Needs a strong GPU."
    Ultra = "4x supersampling. Very heavy."
}

$s = @{ Look = "BS2"; Fps = "60"; Exposure = ""; Gamma = ""; Scale = "Look default"; Bots = "7"; Hunt = $true }
if (Test-Path $Cfg) { try { (Get-Content $Cfg -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $s[$_.Name] = $_.Value } } catch {} }

function New-Ctl($type, $x, $y, $w, $h, $text) {
    $c = New-Object $type; $c.Location = New-Object Drawing.Point($x, $y); $c.Size = New-Object Drawing.Size($w, $h)
    if ($text) { $c.Text = $text }; $form.Controls.Add($c); $c
}
$form = New-Object Windows.Forms.Form
$form.Text = "BioShock 2 Recomp"; $form.ClientSize = New-Object Drawing.Size(460, 420)
$form.FormBorderStyle = "FixedDialog"; $form.MaximizeBox = $false; $form.StartPosition = "CenterScreen"
$form.BackColor = [Drawing.Color]::FromArgb(18, 28, 24); $form.ForeColor = [Drawing.Color]::FromArgb(200, 230, 210)
$form.Font = New-Object Drawing.Font("Segoe UI", 9.5)

$title = New-Ctl Windows.Forms.Label 16 10 430 30 "BioShock 2  -  Xbox 360 recomp"
$title.Font = New-Object Drawing.Font("Segoe UI Semibold", 15)

New-Ctl Windows.Forms.Label 16 52 90 22 "Look" | Out-Null
$cbLook = New-Ctl Windows.Forms.ComboBox 110 50 140 24 $null; $cbLook.DropDownStyle = "DropDownList"
$Looks | ForEach-Object { [void]$cbLook.Items.Add($_) }; $cbLook.SelectedItem = $s.Look
$lbInfo = New-Ctl Windows.Forms.Label 110 76 340 20 ""; $lbInfo.ForeColor = [Drawing.Color]::FromArgb(140, 170, 150)
$cbLook.Add_SelectedIndexChanged({ $lbInfo.Text = $LookInfo[$cbLook.SelectedItem] }); $lbInfo.Text = $LookInfo[$cbLook.SelectedItem]

New-Ctl Windows.Forms.Label 16 104 90 22 "Frame rate" | Out-Null
$cbFps = New-Ctl Windows.Forms.ComboBox 110 102 140 24 $null; $cbFps.DropDownStyle = "DropDownList"
@("60", "30 (original)") | ForEach-Object { [void]$cbFps.Items.Add($_) }; $cbFps.SelectedIndex = $(if ($s.Fps -eq "30") { 1 } else { 0 })

New-Ctl Windows.Forms.Label 16 136 90 22 "Resolution" | Out-Null
$cbScale = New-Ctl Windows.Forms.ComboBox 110 134 140 24 $null; $cbScale.DropDownStyle = "DropDownList"
@("Look default", "1 (720p)", "2 (1440p)", "3 (2160p)", "4 (2880p)") | ForEach-Object { [void]$cbScale.Items.Add($_) }
$cbScale.SelectedItem = $(if ($cbScale.Items.Contains($s.Scale)) { $s.Scale } else { "Look default" })

New-Ctl Windows.Forms.Label 16 168 90 22 "Brightness" | Out-Null
$tbExp = New-Ctl Windows.Forms.TextBox 110 166 60 24 $s.Exposure
New-Ctl Windows.Forms.Label 176 168 56 22 "Gamma" | Out-Null
$tbGam = New-Ctl Windows.Forms.TextBox 236 166 60 24 $s.Gamma
$lbHint = New-Ctl Windows.Forms.Label 304 168 150 22 "blank = look default"; $lbHint.ForeColor = $lbInfo.ForeColor

$status = New-Ctl Windows.Forms.Label 16 390 430 22 ""
function Set-Status { if (Test-Path $Exe) { $status.Text = "Ready." ; $status.ForeColor = [Drawing.Color]::FromArgb(120, 220, 140) }
                      else { $status.Text = "Not built yet: press Build + Play."; $status.ForeColor = [Drawing.Color]::Orange } }
Set-Status

function Save-Settings {
    $s.Look = $cbLook.SelectedItem; $s.Fps = $(if ($cbFps.SelectedIndex -eq 1) { "30" } else { "60" })
    $s.Scale = $cbScale.SelectedItem; $s.Exposure = $tbExp.Text.Trim(); $s.Gamma = $tbGam.Text.Trim()
    $s.Bots = [string]$cbBots.SelectedItem; $s.Hunt = $chkHunt.Checked
    New-Item -ItemType Directory (Split-Path $Cfg) -Force | Out-Null
    $s | ConvertTo-Json | Set-Content $Cfg
}
function Get-PlayArgs {
    $a = "-Project bioshock2 -GameDir game_bs2 -Look $($s.Look)"
    if ($s.Fps -eq "30") { $a += " -Fps30" }
    if ($s.Scale -match "^(\d)") { $a += " -Scale $($Matches[1])" }
    $d = 0.0
    if ($s.Exposure -and [double]::TryParse($s.Exposure, [ref]$d)) { $a += " -Exposure $d" }
    if ($s.Gamma -and [double]::TryParse($s.Gamma, [ref]$d)) { $a += " -Gamma $d" }
    $a
}
function Start-Console([string]$cmd, [switch]$Keep) {
    $flags = @("-NoProfile", "-ExecutionPolicy", "Bypass")
    if ($Keep) { $flags += "-NoExit" }
    # Errors stay on screen instead of the window vanishing.
    $wrapped = "try { `$ErrorActionPreference = 'Stop'; $cmd } catch { Write-Host `$_ -ForegroundColor Red; Read-Host 'Press Enter to close' }"
    Start-Process powershell -WorkingDirectory $Root -ArgumentList ($flags + @("-Command", $wrapped))
}

$btnPlay = New-Ctl Windows.Forms.Button 16 206 140 38 "Play"
$btnPlay.Font = New-Object Drawing.Font("Segoe UI Semibold", 11); $btnPlay.BackColor = [Drawing.Color]::FromArgb(40, 120, 60)
$btnPlay.Add_Click({
    Save-Settings
    if (-not (Test-Path $Exe)) { [Windows.Forms.MessageBox]::Show("bioshock2.exe isn't built yet. Use Build + Play.", "BioShock 2 Recomp") | Out-Null; return }
    Start-Console "& '.\tools\play.ps1' $(Get-PlayArgs)"
})
$btnBuild = New-Ctl Windows.Forms.Button 164 206 140 38 "Build + Play"
$btnBuild.Add_Click({ Save-Settings; Start-Console "& '.\go.ps1' -Look $($s.Look)$(if ($s.Fps -eq '30') { ' -Fps30' })" -Keep })
$btnReport = New-Ctl Windows.Forms.Button 312 206 132 38 "Last run report"
$btnReport.Add_Click({ Start-Console "& '.\tools\report.ps1'" -Keep })
$btnLogs = New-Ctl Windows.Forms.Button 16 252 140 32 "Open logs folder"
$btnLogs.Add_Click({ $l = Join-Path $Root "logs"; New-Item -ItemType Directory $l -Force | Out-Null; Start-Process explorer $l })
$btnFix = New-Ctl Windows.Forms.Button 164 252 140 32 "Crash-fix loop (dev)"
$btnFix.Add_Click({ Start-Console ". '.\tools\vsenv.ps1'; & '.\tools\fix_loop.ps1' -Project bioshock2 -GameDir game_bs2 -Look" -Keep })
$btnHelp = New-Ctl Windows.Forms.Button 312 252 132 32 "In-game keys"
$btnHelp.Add_Click({ [Windows.Forms.MessageBox]::Show("F9   Cheat panel: god mode, noclip, fly, teleport, game speed, give health / EVE / ammo / plasmids, summon, custom console commands, memory name scanner`nD-pad Left x3   MP weapon wheel (right stick aims, A equips, B cancels; keyboard G)`nF4   Emulator settings`nF3   Debug overlay`n`nThe panel needs the engine captured first: open and close the pause menu once, and pick up any item once before using Give.`n`nTest cheats on a spare save.", "BioShock 2 Recomp") | Out-Null })
foreach ($b in @($btnBuild, $btnReport, $btnLogs, $btnFix, $btnHelp)) { $b.BackColor = [Drawing.Color]::FromArgb(36, 52, 44) }
foreach ($b in @($btnPlay, $btnBuild, $btnReport, $btnLogs, $btnFix, $btnHelp)) { $b.FlatStyle = "Flat"; $b.FlatAppearance.BorderColor = [Drawing.Color]::FromArgb(70, 110, 85) }
foreach ($c in @($cbLook, $cbFps, $cbScale, $tbExp, $tbGam)) { $c.BackColor = [Drawing.Color]::FromArgb(30, 44, 38); $c.ForeColor = $form.ForeColor }

# ---- Multiplayer: offline private matches against AI bots (mp\, built by Install.ps1 or mp\tools\mp_init.ps1)
$mpTitle = New-Ctl Windows.Forms.Label 16 296 430 22 "Multiplayer  -  offline private match vs AI bots"
$mpTitle.Font = New-Object Drawing.Font("Segoe UI Semibold", 10.5)
New-Ctl Windows.Forms.Label 16 326 40 22 "Bots" | Out-Null
$cbBots = New-Ctl Windows.Forms.ComboBox 58 324 50 24 $null; $cbBots.DropDownStyle = "DropDownList"
1..9 | ForEach-Object { [void]$cbBots.Items.Add([string]$_) }
$cbBots.SelectedItem = $(if ($cbBots.Items.Contains([string]$s.Bots)) { [string]$s.Bots } else { "7" })
$chkHunt = New-Ctl Windows.Forms.CheckBox 118 324 120 24 "Bots hunt me"; $chkHunt.Checked = [bool]$s.Hunt
$btnMp = New-Ctl Windows.Forms.Button 244 320 200 32 "Play Multiplayer"
$btnMp.BackColor = [Drawing.Color]::FromArgb(40, 120, 60); $btnMp.FlatStyle = "Flat"
$lbMp = New-Ctl Windows.Forms.Label 16 358 430 30 "In game: Prologue > Player Lobby > Private Match > Start. Bots join by themselves. Offline only, never Xbox Live."
$lbMp.ForeColor = $lbInfo.ForeColor; $lbMp.Font = New-Object Drawing.Font("Segoe UI", 8.5)
function Set-BotIni {
    $cfg = Join-Path $MpRoot "config\mp_bots.ini"
    if (-not (Test-Path $MpIni) -and (Test-Path $cfg)) { New-Item -ItemType Directory (Split-Path $MpIni) -Force | Out-Null; Copy-Item $cfg $MpIni }
    if (-not (Test-Path $MpIni)) { return }
    $t = [IO.File]::ReadAllText($MpIni)
    $t = [regex]::Replace($t, '(?m)^count=.*$', "count=$($s.Bots)")
    $t = [regex]::Replace($t, '(?m)^hunters=.*$', "hunters=$(if ($s.Hunt) { 'all' } else { '0' })")
    [IO.File]::WriteAllText($MpIni, $t)
}
$btnMp.Add_Click({
    Save-Settings; Set-BotIni
    if (-not (Test-Path $MpExe)) {
        $r = [Windows.Forms.MessageBox]::Show("Multiplayer isn't built yet. Build it now? (10-30 min, needs the single-player build done first)", "BioShock 2 Recomp", "YesNo")
        if ($r -eq "Yes") { Start-Console "& '.\mp\tools\mp_init.ps1' -NoImport" -Keep }
        return
    }
    $a = (Get-PlayArgs) -replace '^-Project bioshock2 ', '-Project bioshock2mp '
    Start-Console "& '.\mp\tools\play.ps1' $a -Extra '--fake_live_signin=true'"
})
$cbBots.Add_SelectedIndexChanged({ Save-Settings; Set-BotIni }); $chkHunt.Add_CheckedChanged({ Save-Settings; Set-BotIni })   # live: the game re-reads it every 2 s
$cbBots.BackColor = [Drawing.Color]::FromArgb(30, 44, 38); $cbBots.ForeColor = $form.ForeColor

$form.Add_FormClosing({ Save-Settings })
[void]$form.ShowDialog()
