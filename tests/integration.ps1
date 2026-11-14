param([string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC", [switch]$DarkTheme)
$ErrorActionPreference = 'Stop'
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before running the integration test.' }
if (!(Test-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/libvlc_addon_plugin.dll'))) { throw 'Build the addon before running this test.' }
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/window-tools.ps1"
$vlc = Join-Path $VlcDirectory 'vlc.exe'
$testDir = Join-Path $root ('build/integration-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$previousPluginPath = $env:VLC_PLUGIN_PATH
$previousDataDir = $env:VLC_ADDON_DATA_DIR
$env:VLC_PLUGIN_PATH = Join-Path $root 'build'
$env:VLC_ADDON_DATA_DIR = Join-Path $testDir 'data'
$config = Join-Path $testDir 'vlcrc'
$log = Join-Path $testDir 'vlc.log'

function Wait-Until([scriptblock]$Check, [string]$Label, [int]$Seconds = 8) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        $value = & $Check
        if ($value) { return $value }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Label. Test files are in $testDir."
}

function PlayerWindow([int]$Id, [string]$Pattern) {
    [TestWindows]::ForProcess($Id) | Where-Object { [TestWindows]::Text($_) -like $Pattern } | Select-Object -First 1
}

function Read-AddonData([string]$File) {
    $path = Join-Path $env:VLC_ADDON_DATA_DIR $File
    if (Test-Path -LiteralPath $path) {
        try { Get-Content -Raw -LiteralPath $path | ConvertFrom-Json } catch [System.IO.IOException] { }
    }
}

function Start-Vlc([string]$Path = '', [switch]$Forward) {
    $options = "--config=`"$config`" --no-media-library --no-qt-privacy-ask --avcodec-hw=none --no-metadata-network-access --no-plugins-cache --file-logging --logfile=`"$log`" --verbose=2"
    if ($Forward) { $options = "--config=" + [char]34 + $config + [char]34 + " --no-media-library --no-qt-privacy-ask --no-plugins-cache" }
    if ($Path) { $options += ' "' + ([Uri]$Path).AbsoluteUri + '"' }
    Start-Process -FilePath $vlc -ArgumentList $options -PassThru
}

function Close-Vlc($Process, [IntPtr]$Window) {
    [TestWindows]::PostMessage($Window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$Process.WaitForExit(8000)) { throw 'VLC did not exit cleanly.' }
    if ($Process.ExitCode -ne 0) { throw "VLC exited with $($Process.ExitCode)." }
}

$sample = Join-Path $testDir 'Example.S01E01.avi'
$next = Join-Path $testDir 'Example.S01E02.avi'
$gap = Join-Path $testDir 'Example.S01E04.avi'
& ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=s=320x180:r=10' -f lavfi -i 'anullsrc=r=44100:cl=stereo' -t 100 -c:a libmp3lame -c:v mpeg4 -metadata title=Example.S01E01 $sample
if ($LASTEXITCODE) { throw 'FFmpeg could not create test media.' }
foreach ($file in @($next, $gap)) {
    & ffmpeg -hide_banner -loglevel error -y -i $sample -c copy -metadata "title=$([IO.Path]::GetFileNameWithoutExtension($file))" $file
    if ($LASTEXITCODE) { throw 'FFmpeg could not create episode fixtures.' }
}
@('[core]', 'extraintf=vlc_addon', 'one-instance=1', 'playlist-enqueue=1', 'qt-continue=0',
  "qt-dark-palette=$([int]$DarkTheme.IsPresent)") |
    Set-Content -LiteralPath $config -Encoding ascii
$p = $null
try {
    $p = Start-Vlc $sample
    $player = Wait-Until { PlayerWindow $p.Id '*Example.S01E01*' } 'the first episode'
    Start-Sleep -Milliseconds 900
    foreach ($index in 1..5) {
        [TestWindows]::Press($player, 0x21)
        Start-Sleep -Milliseconds 130
    }
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 125 } 'five-point speed steps'
    $null = Wait-Until { [TestWindows]::PlaybackStatus($player) -match '^1\.25.\s+.+\d\d:\d\d left$' } 'live toolbar speed and remaining time'

    Start-Sleep -Milliseconds 1200
    [TestWindows]::Press($player, 0x6B)
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 175 } 'NumPad +'
    Start-Sleep -Milliseconds 1200
    [TestWindows]::Press($player, 0x6D)
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 125 } 'NumPad -'
    $indicator = PlayerWindow $p.Id '* left'
    if (!$indicator) { throw 'No interactive playback indicator.' }
    # Locate the speed region in both compact and wide toolbar layouts.
    foreach ($x in 6..79) {
        [TestWindows]::PostMessage($indicator, 0x202, [IntPtr]::Zero, [IntPtr](0x60000 + $x)) | Out-Null
    }
    $speedDialog = Wait-Until { PlayerWindow $p.Id 'Playback speed' } 'the custom speed dialog'
    $null = [TestWindows]::SendMessage([TestWindows]::GetDlgItem($speedDialog, 102), 0xC, [IntPtr]::Zero, '1,37')
    [TestWindows]::Click($speedDialog, 100)
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 137 } 'the saved custom speed'

    [TestWindows]::Press($player, 0x6A)
    $null = Wait-Until { (Read-AddonData 'settings.json').discord_enabled -eq $false } 'the saved Discord toggle'

    $secondProcess = Start-Vlc $gap -Forward
    if (!$secondProcess.WaitForExit(6000)) { throw 'Explorer-style file open did not forward to VLC.' }
    if (!(PlayerWindow $p.Id '*Example.S01E01*')) { throw 'Queueing interrupted the current video.' }

    [TestWindows]::Press($player, 0x69)
    $dialog = Wait-Until { PlayerWindow $p.Id 'Add bookmark' } 'the NumPad 9 dialog'
    $null = [TestWindows]::SendMessage([TestWindows]::GetDlgItem($dialog, 102), 0xC, [IntPtr]::Zero, 'Integration bookmark')
    [TestWindows]::Click($dialog, 100)
    $null = Wait-Until { (Read-AddonData 'bookmarks.json').bookmarks.comment -eq 'Integration bookmark' } 'the bookmark on disk'
    Close-Vlc $p $player
    $firstLog = Get-Content -LiteralPath $log -Raw
    if ($firstLog -notmatch "preparsing 'Example.S01E02.avi'") { throw 'The neighboring episode was not queued.' }

    $p = Start-Vlc $sample
    $player = Wait-Until { PlayerWindow $p.Id '*Example.S01E01*' } 'the reopened file'
    $dialog = Wait-Until { PlayerWindow $p.Id 'Continue watching' } 'the resume prompt'
    [TestWindows]::Click($dialog, 100)
    [TestWindows]::Press($player, 0x22)
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 132 } 'the restored rate'
    [TestWindows]::Press($player, 0x24)
    $null = Wait-Until { (Read-AddonData 'settings.json').video_speed -eq 100 } 'Home reset'

    Close-Vlc $p $player
    $p = Start-Vlc
    $null = Wait-Until { PlayerWindow $p.Id '*VLC media player*' } 'VLC opened without a file'
    $latest = Wait-Until { PlayerWindow $p.Id 'Continue watching' } 'the last unfinished video'
    [TestWindows]::Click($latest, 101)
    $secondProcess = Start-Vlc $gap -Forward
    if (!$secondProcess.WaitForExit(6000)) { throw 'File open did not forward to idle VLC.' }
    $player = Wait-Until { PlayerWindow $p.Id '*Example.S01E04*' } 'opening a video in idle VLC'
    Close-Vlc $p $player
    Write-Host "Integration checks passed. Logs and fixtures are in $testDir."
} finally {
    $env:VLC_PLUGIN_PATH = $previousPluginPath
    $env:VLC_ADDON_DATA_DIR = $previousDataDir
    if ($p -and !$p.HasExited -and $p.Path -eq $vlc) {
        if (!$p.CloseMainWindow() -or !$p.WaitForExit(3000)) { Stop-Process -Id $p.Id }
    }
}
