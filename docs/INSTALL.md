# Installation and removal

## Requirements

- Windows 10 or 11, x64.
- [VLC 3.0.24 x64 installer](https://download.videolan.org/vlc/3.0.24/win64/vlc-3.0.24-win64.exe). The plugin builds against this version's internal module API.
- The release installer uses Windows PowerShell. Building from source requires PowerShell 7, internet access, curl, and tar.

## Install or update from a release

1. Close VLC. Under **Assets** on [Releases](https://github.com/smilekritik/vlc-watchkit/releases/latest), download the Windows x64 ZIP and use **Extract All** in File Explorer.
2. Double-click **Install WatchKit.bat** in the extracted folder. Approve the Windows administrator prompt under the account that uses VLC, if prompted.
3. After setup reports success, open VLC and choose **View > WatchKit Settings**.

Setup checks the previous installation path, VLC's registered location, and the default Program Files folder. For a custom folder, run `& '.\Install WatchKit.bat' -VlcDirectory 'D:\Apps\VLC'` from PowerShell. To update, extract the new release and run the same installer. Your WatchKit settings and bookmarks stay in `%LOCALAPPDATA%\VLCAddon`.

## Build and install from source

Clone the repository or download the source archive. If Windows marks the scripts as downloaded files, review them and run `Get-ChildItem ./scripts -Filter *.ps1 -File | Unblock-File`. Open PowerShell 7 in the repository root and run:

```powershell
./scripts/build.ps1 -Test
```

The first build downloads about 180 MiB into `.deps`, verifies the checksums, and writes `build/libvlc_addon_plugin.dll`. Close VLC and run the installer under the Windows account that will use VLC.

```powershell
& '.\Install WatchKit.bat'
```

If your VLC is in a custom folder, pass the same path to both scripts:

```powershell
./scripts/build.ps1 -VlcDirectory 'D:\Apps\VLC' -Test
& '.\Install WatchKit.bat' -VlcDirectory 'D:\Apps\VLC'
```

Make VLC the default Windows app for the video extensions you want to open from File Explorer. The installer registers the module for the current user, enables single-instance enqueue, and replaces VLC's resume prompt with the addon's. Find the addon's settings and bookmark list in **View**.

## Change VLC's theme

Close VLC and run one of these commands from a regular PowerShell 7 window in the repository root:

```powershell
./scripts/set-vlc-theme.ps1 -Theme Dark
./scripts/set-vlc-theme.ps1 -Theme Light
```

Reopen VLC after running the command. Dark mode uses VLC's built-in dark palette with Qt's Fusion style for menus and playback controls. Light mode restores VLC's default system style. WatchKit follows the selected palette in the standard Qt interface, including the playback overlay, resume prompt, bookmark windows, and settings.

![Dark fullscreen controls with the WatchKit speed and remaining-time display](screenshots/vlc-fullscreen-controls.png)

## Back up or remove the addon

Close VLC and run `./scripts/uninstall.ps1` in an administrator PowerShell 7 window. Pass `-VlcDirectory` if you installed into a custom folder. The script removes the addon and restores the managed VLC options when they still match the installed values.

Back up `%LOCALAPPDATA%\VLCAddon` to keep your settings, bookmarks, and viewing positions. Uninstalling leaves those files in place. To remove your data too, delete that folder after uninstalling.

## Troubleshooting

- **Setup could not find its files.** Download the Windows x64 ZIP under **Assets**, use **Extract All**, and run the batch file in the extracted folder.
- **VLC version or architecture mismatch.** Install the VLC 3.0.24 x64 build linked above. If you have multiple copies, specify the folder that contains the right `vlc.exe`.
- **VLC is still running.** Close all VLC windows and exit VLC from the notification area if it is still open. Then rerun the installer.
- **Plugin cache could not be rebuilt.** Check the installer message and repair VLC if the VLC cache tool fails again. The installer attempts to restore the previous files and prints any path it could not restore.
- If the addon's menu is missing, close VLC, rerun the installer, and restart VLC. Confirm that you are using the default Qt interface.
- If the menu exists but its items do nothing, open **Tools > Messages** with verbosity 2. Look for `WatchKit loaded` or errors from `vlc_addon`.
- If VLC cannot load the module after an upgrade, uninstall the addon before upgrading or use the pinned VLC version until the addon supports the new release.
- If a JSON file becomes unreadable, close VLC and copy the file somewhere safe before fixing or moving it. The addon preserves unreadable files and turns Discord publication off if it cannot read settings.
