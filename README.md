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
- **Native Windows tray, macOS menu-bar, and Linux AppIndicator/GTK integration** with notifications, folder selection, proxy settings, start-on-login, and account switching.
- **Automatic Nintendo Account browser return**: after clicking **Select this person**, the browser returns the one-time `npf…://auth` callback to NSO Album Sync automatically when the operating system allows it. Manual link entry remains available as a fallback when another application owns the callback scheme or registration is unavailable.
- **Secure credential storage** through the operating system credential/keychain service when available. Session tokens are not written to `config.json` in plaintext by current builds.
- **Discord Rich Presence** using Discord's local desktop IPC; game, console, play-time state, artwork, and Nintendo shop link data are published when Nintendo reports that you are playing.
- **Single-instance protection** so accidentally launching the application twice does not create duplicate sync/presence workers. Nintendo auth callbacks launched as a second process are forwarded to the already-running instance instead.

## Nintendo Account and nxapi disclosure

NSO Album Sync uses the third-party [`nxapi-znca-api`](https://github.com/samuelthomas2774/nxapi-znca-api) service at `fancy.org.uk` for Nintendo Switch Online request attestation and request/response encryption.

Before Nintendo Account sign-in begins, the app requires an explicit acknowledgement that the user's Nintendo Account `id_token` is sent to this third-party service. The token can contain Nintendo Account information and can be used to authenticate to Nintendo Switch Online services while valid. The app also identifies itself to nxapi, caches service tokens/version data, serializes automated nxapi work, and respects `Retry-After` backoff.

The Nintendo Account OAuth callback is protected with PKCE and an OAuth `state` value. NSO Album Sync validates that state before exchanging the one-time `session_token_code`. The PKCE verifier remains in the process that opened the Nintendo sign-in page.

## Discord visibility

Rich Presence is submitted to the local Discord desktop client. Discord itself decides who can see activities. If the activity appears for you but not for friends/server members, enable **Discord → User Settings → Activity Sharing → Share my activity** and check any per-server activity privacy settings. NSO Album Sync cannot override Discord privacy controls.

## Downloads

The release is intentionally kept to **one download per operating system**:

- **Windows:** `nso-album-sync.exe` — x64 Windows.
- **macOS:** `nso-album-sync-macOS.zip` — one **Universal 2** app containing both Apple Silicon (`arm64`) and Intel (`x86_64`) code. The final combined app is locally/ad-hoc code-signed and verified after it is assembled.
- **Linux:** `nso-album-sync.AppImage` — x64 Linux.

ARM64 Windows and ARM64 Linux release variants are intentionally not produced. The macOS architecture-specific builds exist only as short-lived CI slices used to create the single Universal 2 application; they are not separate release downloads.

Release downloads and SHA-256 checksums are available on the [Releases](https://github.com/Dycool/NSO-Album-Sync/releases) page.

## Quick start

1. Run the download for your operating system. On first launch, NSO Album Sync opens its Nintendo Account onboarding flow automatically.
2. Read and accept the nxapi third-party disclosure.
3. Nintendo's sign-in page opens in your normal browser. Sign in and click **Select this person**. The browser normally returns directly to NSO Album Sync and authentication completes automatically.
4. If automatic return cannot be registered because another application owns Nintendo's callback scheme, NSO Album Sync shows the original manual redirect-link field instead of replacing that application's handler.
5. After authentication, the app performs the first album sync and then stays in the tray/menu bar. Use the menu to change the album folder, auto-sync, notifications, Discord presence, proxy, or startup behavior.

By default captures are saved under the user's Pictures directory in `Nintendo Switch Album/Album/...` unless another destination is selected.

## Build locally

The project requires CMake 3.20+ and a C++20 compiler. Platform integrations use Win32 on Windows, Cocoa/UserNotifications on macOS, and GTK 3 + Ayatana AppIndicator on Linux when available.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On macOS, CMake ad-hoc signs the generated `.app` after every local build by default (`NSO_MACOS_CODESIGN_IDENTITY=-`) and immediately verifies the signature. Release CI then combines the arm64 and x86_64 app executables with `lipo` and signs/verifies the final Universal 2 bundle again.

## License

This project is licensed under the [MIT License](LICENSE).
