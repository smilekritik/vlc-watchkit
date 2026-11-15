# Architecture and tests

The plugin runs in the VLC process. VLC callbacks signal a single worker that owns playback state, local data, Win32 dialogs, and the Discord connection. The worker waits for VLC events, window messages, IPC completions, and scheduled work. Windows hooks observe keyboard messages only on VLC's own window threads, so keypad codes survive Qt's key translation. The Lua scripts connect VLC's **View** menu to the native module.

| Path | Responsibility |
| --- | --- |
| `src/plugin.cpp` | VLC lifecycle, playlist integration, and feature orchestration |
| `src/model.cpp` | Episode matching, speed steps, time display, and Unicode conversion |
| `src/store.cpp` | Versioned local data and atomic file replacement |
| `src/windows.cpp` | VLC window discovery and focused keyboard handling |
| `src/ui.cpp` | Resume prompt, bookmark dialogs, and settings |
| `src/discord.cpp` | Local IPC framing, retry, and activity updates |

The build uses pinned VLC 3.0.24 headers, llvm-mingw, and nlohmann/json. The build derives the import library from the target VLC installation so the same binary supplies the exports at runtime. The build downloads and verifies dependencies under `.deps`.

To package a release, build the plugin from the tagged revision, then run `./scripts/package-release.ps1 -Tag vX.Y.Z`. Upload the ZIP and `SHA256SUMS.txt` from `build/release/vX.Y.Z` to GitHub Releases. The script checks that the installer is at the archive root and includes the plugin, Lua menus, and third-party licenses.

## Checks

Run the unit suite with `./scripts/build.ps1 -Test`. It covers sequence gaps, ambiguous episodes, ordinary movie filenames, speed steps, time formatting, shortcut persistence, Unicode, and bookmarks whose source file is missing.

Run `./tests/integration.ps1` on an interactive Windows desktop with VLC closed and FFmpeg on PATH. The script uses generated video, a temporary VLC configuration, and isolated local data under `build`. It checks loading, native shortcuts, saved data, opening files through the running instance, and restart behavior.

Before changing VLC versions, also try fullscreen on both monitors if applicable, check native menu speed changes, and test live Discord publication on the intended Discord desktop channel. For a benchmark, compare the same file and renderer in fresh VLC processes. Sample idle CPU and private memory, then measure playback and startup across multiple launches.

To measure idle load, run `./tests/benchmark.ps1`. The script alternates baseline and addon launches and reports startup time, CPU time over each sample, private memory, and working set.

To check the desktop IPC connection, run `./tests/discord-live.ps1 -ApplicationId YOUR_PUBLIC_ID` with Discord running, VLC closed, and FFmpeg on PATH. The test briefly publishes the generated title and verifies that Discord acknowledges it and that NumPad * saves the disabled setting in the test data directory.
