<p align="center">
  <img src="icon.svg" alt="NSO Album Sync icon" width="128" height="128">
</p>

# NSO Album Sync

A lightweight native tray/menu-bar app that keeps the recent Nintendo Switch Online album mirrored to a PC or Mac while preserving the Nintendo Switch USB album layout.

## Features

- **Automatic background sync** with a configurable interval (60 minutes by default) plus **Sync Album Now** on demand.
- **Nintendo USB-compatible layout**: `Album/<Game Name>/YYYYMMDDHHMMSS00_c.jpg` / `.mp4`.
- **Existing-album detection and restoration** so already-backed-up captures are skipped and locally deleted cloud captures can be restored.
- **Title ID and localized-folder matching** to keep using existing Portuguese, Japanese, English, custom, and other localized game folders.
- **Original capture timestamps** preserved on downloaded media (including creation/write time on Windows).
- **Native Windows tray, macOS menu-bar, and Linux AppIndicator/GTK integration** with notifications, folder selection, proxy settings, start-on-boot, and account switching.
- **Secure credential storage** through the operating system credential/keychain service when available. Session tokens are not written to `config.json` in plaintext by current builds.
- **Discord Rich Presence** using Discord's local desktop IPC; game, console, play-time state, artwork, and Nintendo shop link data are published when Nintendo reports that you are playing.
- **Single-instance protection** so accidentally launching the application twice does not create duplicate sync/presence workers.

## Nintendo Account and nxapi disclosure

NSO Album Sync uses the third-party [`nxapi-znca-api`](https://github.com/samuelthomas2774/nxapi-znca-api) service at `fancy.org.uk` for Nintendo Switch Online request attestation and request/response encryption.

Before Nintendo Account sign-in begins, the app requires an explicit acknowledgement that the user's Nintendo Account `id_token` is sent to this third-party service. The token can contain Nintendo Account information and can be used to authenticate to Nintendo Switch Online services while valid. The app also identifies itself to nxapi, caches service tokens/version data, serializes automated nxapi work, and respects `Retry-After` backoff.

## Discord visibility

Rich Presence is submitted to the local Discord desktop client. Discord itself decides who can see activities. If the activity appears for you but not for friends/server members, enable **Discord → User Settings → Activity Sharing → Share my activity** and check any per-server activity privacy settings. NSO Album Sync cannot override Discord privacy controls.

## Downloads

GitHub Actions builds both x64 and ARM64 where the platform supports them:

- **Windows:** native x64 and ARM64 `.exe` builds, plus one multi-architecture archive containing both executables. A normal Windows PE `.exe` has one machine architecture, so the two machine-code targets cannot be truthfully combined into one ordinary executable.
- **macOS:** one **Universal 2** `.app` whose executable contains both `arm64` and `x86_64` slices. The final combined app is ad-hoc/local code-signed and verified after `lipo` packaging.
- **Linux:** native x64 and ARM64 **AppImages**, plus one multi-architecture archive containing both AppImages. The AppImage runtime itself is architecture-specific, so a single standard AppImage cannot boot on both CPU architectures.

Release downloads and SHA-256 checksums are available on the [Releases](https://github.com/Dycool/NSO-Album-Sync/releases) page.

## Quick start

1. Run the build for your platform. On first launch, NSO Album Sync opens its Nintendo Account onboarding flow automatically.
2. Read and accept the nxapi third-party disclosure.
3. Nintendo's sign-in page opens in your browser. Sign in, then copy the complete link behind **Select this person** and paste it into NSO Album Sync.
4. After authentication, the app performs the first album sync and then stays in the tray/menu bar. Use the menu to change the album folder, auto-sync, notifications, Discord presence, proxy, or startup behavior.

By default captures are saved under the user's Pictures directory in `Nintendo Switch Album/Album/...` unless another destination is selected.

## Build locally

The project requires CMake 3.20+ and a C++20 compiler. Platform integrations use Win32 on Windows, Cocoa/UserNotifications on macOS, and GTK 3 + Ayatana AppIndicator on Linux when available.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On macOS, CMake ad-hoc signs the generated `.app` after every local build by default (`NSO_MACOS_CODESIGN_IDENTITY=-`) and immediately verifies the signature.

## License

This project is licensed under the [MIT License](LICENSE).
