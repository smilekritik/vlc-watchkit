#Requires -RunAsAdministrator
param([string]$VlcDirectory = "$env:ProgramFiles\VideoLAN\VLC")
$ErrorActionPreference = 'Stop'
if (Get-Process vlc -ErrorAction SilentlyContinue) { throw 'Close VLC before installing the addon.' }
$player = Get-Item -LiteralPath (Join-Path $VlcDirectory 'vlc.exe')
if ($player.VersionInfo.ProductVersion -notmatch '^3[.,]0[.,]24(?:[.,]|$)') { throw 'Install VLC 3.0.24 x64 first.' }
$bytes = [IO.File]::ReadAllBytes($player.FullName)
if ([BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 60) + 4) -ne 0x8664) { throw 'The addon requires x64 VLC.' }

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'build/libvlc_addon_plugin.dll'
if (!(Test-Path -LiteralPath $dll)) { throw 'Build the DLL first with scripts/build.ps1.' }
$configDirectory = Join-Path $env:APPDATA 'vlc'
$dataDirectory = Join-Path $env:LOCALAPPDATA 'VLCAddon'
New-Item -ItemType Directory -Path $configDirectory,$dataDirectory -Force | Out-Null
$preferences = Join-Path $configDirectory 'vlcrc'
$receipt = Join-Path $dataDirectory 'installation.json'
$plugin = Join-Path $VlcDirectory 'plugins/control/libvlc_addon_plugin.dll'
$libraries = Join-Path $configDirectory 'lua/extensions'
New-Item -ItemType Directory -Path $libraries -Force | Out-Null

$encoding = [Text.UTF8Encoding]::new($false)
$content = if (Test-Path -LiteralPath $preferences) { [IO.File]::ReadAllText($preferences) } else { "[core]`r`n" }
$changes = [ordered]@{'extraintf' = ''; 'one-instance' = '1'; 'playlist-enqueue' = '1'; 'qt-continue' = '0'}
$prior = [ordered]@{}
$activeSettings = @{}
foreach ($name in $changes.Keys) {
    $pattern = '(?m)^' + [regex]::Escape($name) + '=([^\r\n]*)'
    $activeSettings[$name] = [regex]::new($pattern, [Text.RegularExpressions.RegexOptions]::RightToLeft)
    $match = $activeSettings[$name].Match($content)
    $prior[$name] = if ($match.Success) { $match.Groups[1].Value } else { $null }
}
$extras = @($prior['extraintf'] -split ':' | Where-Object { $_ -and $_ -ne 'vlc_addon' })
$changes['extraintf'] = ($extras + @('vlc_addon')) -join ':'
if (!(Test-Path -LiteralPath $receipt)) {
    if (Test-Path -LiteralPath $preferences) { Copy-Item -LiteralPath $preferences (Join-Path $dataDirectory 'vlcrc.before-install') }
    @{ version = 1; vlc = $VlcDirectory; original = $prior } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
}

Copy-Item -LiteralPath $dll -Destination $plugin -Force
$menuScripts = Get-ChildItem -LiteralPath (Join-Path $root 'lua') -Filter 'vlc_addon_*.lua'
foreach ($menu in $menuScripts) { Copy-Item -LiteralPath $menu.FullName -Destination $libraries -Force }
foreach ($name in $changes.Keys) {
    $replacement = "$name=$($changes[$name])"
    $setting = $activeSettings[$name]
    $effective = $setting.Match($content)
    if ($effective.Success) {
        # VLC reads active settings in file order. Keep earlier values as comments.
        $content = $setting.Replace($content, {
            param($match)
            if ($match.Index -eq $effective.Index) { $replacement }
            else { '#' + $match.Value }
        })
        continue
    }
    $comment = [regex]::new('(?m)^#' + [regex]::Escape($name) + '=[^\r\n]*')
    if ($comment.IsMatch($content)) { $content = $comment.Replace($content, { param($match) $replacement }, 1) }
    else { $content = $content.TrimEnd() + "`r`n$replacement`r`n" }
}
[IO.File]::WriteAllText($preferences, $content, $encoding)
& (Join-Path $VlcDirectory 'vlc-cache-gen.exe') (Join-Path $VlcDirectory 'plugins')
if ($LASTEXITCODE) { throw 'VLC could not regenerate its plugin cache. See docs/INSTALL.md.' }
Write-Host 'VLC WatchKit is installed. Launch VLC and open View > VLC WatchKit Settings.'
