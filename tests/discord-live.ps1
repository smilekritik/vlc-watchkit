param(
    [Parameter(Mandatory)][string]$ApplicationId,
    [string]$LargeImage = '',
    [string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC"
)
$ErrorActionPreference = 'Stop'
if ($ApplicationId -notmatch '^\d{16,20}$') { throw 'Use a public numeric Discord Application ID.' }
if (!(Get-Process Discord,DiscordCanary,DiscordPTB -ErrorAction SilentlyContinue)) {
    throw 'Start the Discord desktop client before the live test.'
}
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before running the live test.' }
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/window-tools.ps1"
$directory = Join-Path $root ('build/discord-test-' + [guid]::NewGuid().ToString('N'))
$previousPlugin = $env:VLC_PLUGIN_PATH
$previousData = $env:VLC_ADDON_DATA_DIR
$env:VLC_PLUGIN_PATH = Join-Path $root 'build'
$env:VLC_ADDON_DATA_DIR = Join-Path $directory 'data'
$null = New-Item -ItemType Directory -Path $env:VLC_ADDON_DATA_DIR
$log = Join-Path $directory 'vlc.log'
$file = Join-Path $directory 'VLC WatchKit test video.avi'
$config = Join-Path $directory 'vlcrc'
$p = $null
try {
    & ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=s=320x180:r=10' -f lavfi -i 'anullsrc=r=44100:cl=stereo' -t 45 -c:a libmp3lame -c:v mpeg4 -metadata 'title=VLC WatchKit test video' $file
    if ($LASTEXITCODE) { throw 'FFmpeg could not create test media.' }
    @{
        version = 2; discord_enabled = $true; application_id = $ApplicationId; large_image = $LargeImage
        bindings = @(
            @{key=106;modifiers=0},@{key=105;modifiers=0},@{key=105;modifiers=2},
            @{key=33;modifiers=0},@{key=34;modifiers=0},@{key=36;modifiers=0},
            @{key=107;modifiers=0},@{key=109;modifiers=0},@{key=84;modifiers=0}
        )
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8NoBOM (Join-Path $env:VLC_ADDON_DATA_DIR 'settings.json')
    @('[core]', 'qt-continue=0', 'extraintf=vlc_addon') | Set-Content -Encoding ascii $config
    $uri = ([Uri]$file).AbsoluteUri
    $arguments = "--config=`"$config`" --no-media-library --no-qt-privacy-ask --no-metadata-network-access --no-plugins-cache --file-logging --logfile=`"$log`" --verbose=2 --play-and-exit `"$uri`""
    $p = Start-Process (Join-Path $VlcDirectory 'vlc.exe') -ArgumentList $arguments -PassThru
    for ($index = 0; $index -lt 60; ++$index) {
        $windows = [TestWindows]::ForProcess($p.Id)
        $window = $windows | Where-Object { [TestWindows]::Text($_) -like 'VLC WatchKit test video*' } | Select-Object -First 1
        if ($window) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!$window) { throw "Test playback window did not open. See $log." }
    Start-Sleep -Seconds 26
    [TestWindows]::Press($window, 0x20)
    Start-Sleep -Seconds 2
    [TestWindows]::Press($window, 0x20)
    Start-Sleep -Seconds 2
    [TestWindows]::Press($window, 0x6A)
    Start-Sleep -Seconds 2
    [TestWindows]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$p.WaitForExit(8000)) { throw 'VLC failed to exit.' }
    $text = Get-Content -Raw -LiteralPath $log
    if ([regex]::Matches($text, 'Discord accepted the Rich Presence activity').Count -lt 6 -or $text -match 'Discord rejected the Rich Presence activity') {
        throw "Discord did not acknowledge the playback updates. See $log."
    }
    $settings = Get-Content -Raw (Join-Path $env:VLC_ADDON_DATA_DIR 'settings.json') | ConvertFrom-Json
    if ($settings.discord_enabled) { throw 'NumPad * did not save the disabled setting.' }
    Write-Host "Discord acknowledged playback updates across the 20-second limit window, and NumPad * saved the disabled setting. See $log."
} finally {
    $env:VLC_PLUGIN_PATH = $previousPlugin
    $env:VLC_ADDON_DATA_DIR = $previousData
    if ($p -and !$p.HasExited -and (!$p.CloseMainWindow() -or !$p.WaitForExit(4000))) {
        Stop-Process -Id $p.Id
    }
}
