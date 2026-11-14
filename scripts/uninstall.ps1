#Requires -RunAsAdministrator
param([string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC")
$ErrorActionPreference = 'Stop'
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before uninstalling the addon.' }
$configDirectory = Join-Path $env:APPDATA 'vlc'
$dataDirectory = Join-Path $env:LOCALAPPDATA 'VLCAddon'
$preferences = Join-Path $configDirectory 'vlcrc'
$receipt = Join-Path $dataDirectory 'installation.json'
$history = if (Test-Path -LiteralPath $receipt) { Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json }
if ($history -and $history.vlc -ne $VlcDirectory) { throw "Pass the installed VLC folder via -VlcDirectory." }
if (Test-Path -LiteralPath $preferences) {
    $content = [IO.File]::ReadAllText($preferences)
    $extra = [regex]::new('(?m)^extraintf=([^\r\n]*)')
    $content = $extra.Replace($content, {
        param($match)
        $modules = @($match.Groups[1].Value -split ':' | Where-Object { $_ -and $_ -ne 'vlc_addon' })
        if ($modules.Count) { 'extraintf=' + ($modules -join ':') } else { '#extraintf=' }
    }, 1)
    if ($history) {
        $installed = [ordered]@{'one-instance' = '1'; 'playlist-enqueue' = '1'; 'qt-continue' = '0'}
        $defaults = @{'one-instance' = '0'; 'playlist-enqueue' = '0'; 'qt-continue' = '1'}
        foreach ($name in $installed.Keys) {
            $pattern = '(?m)^' + [regex]::Escape($name) + '=' + $installed[$name] + '(?=\r?$)'
            $value = $history.original.$name
            $replacement = if ($null -eq $value) { "#$name=$($defaults[$name])" } else { "$name=$value" }
            $content = [regex]::new($pattern).Replace($content, {param($match) $replacement}, 1)
        }
    }
    [IO.File]::WriteAllText($preferences, $content, [Text.UTF8Encoding]::new($false))
}
$files = @((Join-Path $VlcDirectory 'plugins/control/libvlc_addon_plugin.dll'),
           (Join-Path $configDirectory 'lua/extensions/vlc_addon_settings.lua'),
           (Join-Path $configDirectory 'lua/extensions/vlc_addon_bookmarks.lua'),
           (Join-Path $configDirectory 'lua/extensions/vlc_addon_continue.lua'))
foreach ($file in $files) { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file } }
& (Join-Path $VlcDirectory 'vlc-cache-gen.exe') (Join-Path $VlcDirectory 'plugins')
if ($LASTEXITCODE) { throw 'VLC could not regenerate its plugin cache.' }
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
Write-Host "WatchKit is uninstalled. Bookmarks and settings are in $dataDirectory."
