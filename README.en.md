# DSC — a companion chat client in pure C

**DSC stands for DeepSeek Chat.** A **pure C** local companion chat client for Windows desktop (Win32 + GDI) and Android. You bring your own DeepSeek API key; no third-party server sits in between.

中文说明见 [README.md](README.md).

- Two modes: **Q&A mode** (multiple threads, list/search/pin/rename/delete) and **Chat mode** (a single companion thread, QQ-style three-tab layout: Me / Settings / Chat)
- Companionship persona with prompt versioning, user-profile variables injected into prompts, and plugin-provided prompt fragments
- Conversation export/import as JSON with checksum and three conflict policies, plus Markdown and plain-text export
- Plugins as native shared libraries (Windows DLL / Android `.so`, plain C ABI) that run in a separate process, so a crash cannot take the app down
- Real network access: WinHTTP on Windows, HttpURLConnection on Android; responses stream token by token, and generation can be stopped or regenerated
- Data management: full backup (database + avatars + plugin data + checksum manifest), one-click restore, old-backup cleanup
- Update flow: check on start and on a fixed interval → resumable download → hash/version check → in-place install → rollback on failure
- Model catalogue: two built-in identifiers plus a `models.json` in the data directory to add or override models and prices
- Long threads stay responsive: messages load 100 per page, and exports can cover the current thread, everything, or the last seven days, optionally with avatars
- All four plugin extension points are wired (prompt fragment, panel, request rewrite, tool declarations); the manifest's permissions and entry hash are shown before installing
- Two-device pairing and live sync: LAN discovery and a direct connection, each side shows a 6-digit code that the other must enter, and after pairing incremental events flow both ways (logical-clock ordering, losing side kept as a copy)
- First run asks for the API key before the mode choice, and the key goes straight into the OS secure store; a demo mode lets you look around without one
- Built-in offline knowledge base (5 groups, 15 articles): chapter tree, full-text search, bookmarks, recently read, and "cite this paragraph" that injects just that paragraph plus its source — never the whole article
- Error center: failures go to their own table and a redacted detail file, never into the message table, the prompt context, or the chat bubble; the chat only shows a quiet hint bar above the input box
- Per-capability consent: time (required), location, weather, gallery image, installed apps and local files are granted and revoked one by one, and raw content is never uploaded
- Editing an AI message rebuilds its memories: the same transaction deletes the old entries by source message and re-extracts them, then publishes a `memory_replace` event for the paired device

Implementation-level design is in `docs/DESIGN.md`; day-to-day usage is in `docs/ADVANCED.md`.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core` | Core layer: mode router, chat engine, message codec, storage, settings, import/export, plugin host, update, backup, two-device pairing and incremental sync, knowledge base, error center, capability broker, memory index |
| `src/platform/win` | Windows adapters: WinHTTP transport, Winsock LAN transport, credential store, plugin child process, GDI+ avatar, system notifications, install/rollback |
| `src/platform/android` | Android adapters: BSD socket LAN transport, Keystore-style credentials (network and UI live on the Java side; C goes through JNI) |
| `src/ui/win` | Windows UI layer (Win32 + GDI, driven by theme tokens; network and import/export run on worker threads) |
| `src/plugin_runner` | Plugin host process and the public ABI header |
| `src/installer` | Self-extracting installer and uninstaller (pure C) |
| `src/cli` | Command-line tool for diagnostics and benchmarks |
| `third_party/sqlite` | Vendored SQLite amalgamation (no external runtime) |
| `tools` | Packaging and screenshot helper scripts |
| `plugins` | Sample plugin |

## Download

Each version ships the packages below (produced under `dist\` for a local build):

| File | What it is |
| --- | --- |
| `DSC-0.1.2-win-x64-portable.zip` | Windows x64 portable build, unpack and run `DSC.exe` |
| `DSC-0.1.2-win-x64-setup.exe` | Windows x64 installer (supports `/S` silent install) |
| `DSC-0.1.2-win-arm64-portable.zip` | Windows ARM64 portable build (cross-compiled) |
| `DSC-0.1.2-android-arm64.apk` | Android package (arm64-v8a, Android 7.0+; 16 KB page aligned) |

Every package ships a matching `.sha256` file. On Android, upgrading from 0.1.1 to 0.1.2 **requires uninstalling the old build first** (the signing certificate changed; Android refuses to install over a different signature).

## Build

Requirements: Windows 10/11 x64 with a MinGW-w64 (MSYS2 ucrt64) toolchain. No MSVC, no CMake, and no interpreter runtime is needed by the shipped binaries.

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
.\build\DSC.exe
~~~

Default flags are `-std=c11 -O2 -Wall -Wextra`. The command-line tool is built with `-Target cli` into `build\dsc-cli.exe`.

## Android

Requirements: Android SDK (build-tools 37.0.0 + platforms/android-35), NDK r27c, JDK 21. **No Gradle** — the script packs with javac + d8 + aapt2 + zipalign + apksigner.

~~~powershell
python tools\build_android.py     # build_android\DSC.apk (arm64-v8a, v2/v3 self-signed)
~~~

## Windows packages

~~~powershell
python tools\package_win.py          # dist\DSC-0.1.2-win-x64-portable.zip (+ .sha256)
python tools\build_arm64.py          # cross-compile Windows ARM64 artifacts into build\arm64\
python tools\package_win.py --arm64  # dist\DSC-0.1.2-win-arm64-portable.zip
python tools\make_installer.py       # dist\DSC-0.1.2-win-x64-setup.exe (+ .sha256)
~~~

The portable zip unpacks and runs; it contains the GUI, the CLI, the sample plugin, the docs and `install.ps1` (creates a Start-menu shortcut without admin rights). The installer is self-extracting, installs to `%LOCALAPPDATA%\Programs\DSC` by default, supports `/S` and `/D=<dir>`, and ships a matching silent uninstaller.

## Documentation

| File | Contents |
| --- | --- |
| `docs/DESIGN.md` | Implementation-level design: layering, data model, state machines, chat pipeline, cross-platform strategy |
| `docs/ADVANCED.md` | Prompt variables, the two modes, export/import, paging, model config, updates and data management |
| `docs/PLUGIN.md` | Plugin manifest fields, extension points, C ABI, host protocol, troubleshooting |
| `docs/RELEASE.md` | Version policy and the release checklist |
| `DEPENDENCIES.md` | Runtime and build-time dependencies, what is deliberately not used |
| `CHANGELOG.md` | Changelog, one section per released version |

## Data and privacy

- The API key lives only in the OS credential store (Windows Credential Manager / Android Keystore-backed storage); neither the UI nor logs show it, and exports never contain credentials.
- Conversation data stays in the app-private directory (`%LOCALAPPDATA%\DSC`; portable mode honours `DSC_DATA_DIR`).
- Requests go directly to the model endpoint you configure. This project collects nothing.

## License

Apache License 2.0 (SPDX: `Apache-2.0`) — see [LICENSE](LICENSE). You may use, modify, relicense and redistribute the code, provided you keep the copyright and license notices, state your changes, and ship a copy of the license; an express patent grant is included. Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).
