# Nintendo Switch Online (NSO) Album Sync

A lightweight, zero-UI background application for Windows, macOS, and Linux that automatically syncs your uploaded Nintendo Switch screenshots and videos directly to your PC, matching the official Switch USB transfer folder hierarchy.

[![Release](https://img.shields.io/github/v/release/Dycool/nso-album-sync?color=blue&style=flat-square)](https://github.com/Dycool/nso-album-sync/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-orange.svg?style=flat-square)](#)

---

## 🌟 Features

- **🔄 Hourly Background Auto-Sync**: Runs quietly in the system tray and checks your Nintendo Switch Online album every hour, automatically downloading new screenshots and gameplay videos.
- **📁 Official Nintendo Switch USB Structure**: Saves captures matching the exact directory hierarchy created by the Nintendo Switch during USB PC transfers:
  ```text
  <Your Chosen Folder>/
  └── Album/
      ├── Super Smash Bros. Ultimate/
      │   ├── 2026081514300000_c.jpg
      │   └── 2026081515000000_c.mp4
      ├── The Legend of Zelda Breath of the Wild/
      │   └── 2026081512100000_c.jpg
      └── Mario Kart 8 Deluxe/
          └── 2026081511050000_c.jpg
  ```
- **💾 Pre-Existing USB Transfer Recognition**: Point the app to your existing Switch album folder and it will immediately detect every photo and video you already backed up, downloading 0 redundant files.
- **🆔 Dynamic Title ID ⟷ Local Folder Learning**: Automatically pairs universal Nintendo Title IDs with your existing local folder names (e.g. Portuguese, Japanese, or custom folder names) so new captures always route into your existing directories without creating duplicate folders.
- **🕒 Original Timestamp Preservation**: Sets filesystem creation and last-modified timestamps to match the exact moment the media was captured on the console RTC hardware.
- **🔄 Local File Restoration**: If you delete a local capture that is still present in your online cloud album (within the recent 100 captures), the next sync will automatically restore and re-download it.
- **🔒 Encrypted Credential Storage**: Session tokens are encrypted on disk using **Windows DPAPI** (`ProtectedData` with `DataProtectionScope.CurrentUser`) on Windows, and locked to `0600` user permissions on macOS and Linux.
- **🖥️ Zero-UI System Tray App**: Silent taskbar notification icon with native context menu:
  - 🔄 **Sync Now** — Trigger an immediate sync on demand.
  - ⏱ **Auto-Sync (Hourly)** — Toggle automatic 60-minute background sync.
  - 📁 **Select Folder...** — Change where your album is stored.
  - 📂 **Open Album Folder** — Open Windows Explorer directly to your album.
  - 🚀 **Start on Boot** — Toggle launch on Windows startup.
  - 🔑 **Sign In / Sign Out** — Manage or switch Nintendo Accounts.
  - ❌ **Exit** — Close the background application.
- **🪶 Ultra-Lightweight**: Single-file executable (~290 KB) with minimal memory footprint (~25 MB RAM).
- **🛡️ `nxapi-znca-api` Compliant**: Strictly follows open-source maintainer guidelines with 2-hour Coral session caching, dynamic version discovery, combined `/f` attestation, and proper rate-limiting.

---

## 🚀 Download & Installation

### Windows
1. Download the latest **`nso-album-sync.exe`** from the [**Releases**](https://github.com/Dycool/nso-album-sync/releases) page.
2. Double-click the executable to launch it into your System Tray (bottom-right of the taskbar).
3. *(Optional)* Click **"🚀 Start on Boot"** in the tray menu to keep it running automatically on startup.

*(Requires [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0). Windows will automatically prompt to download it if not already installed.)*

### macOS & Linux
Standalone single-file binaries are available under [**Releases**](https://github.com/Dycool/nso-album-sync/releases) (macOS binaries are ad-hoc codesigned).

---

## 🔑 Authentication & First-Time Setup

1. Launch **NSO Album Sync**.
2. If not signed in, a one-time setup dialog will appear:
   - Click **"🌐 1. Open Nintendo Sign-In Page"** to open Nintendo's official login page in your default browser.
   - Sign in to your Nintendo Account.
   - Right-click (or copy link) on the **"Select this person"** button (or copy the callback URL from your browser's address bar).
   - Paste the link into the app and click **"✅ Sign In & Connect"**.
3. The app is now connected! It will sit quietly in your system tray and automatically sync new captures.

---

## ⚙️ How nxapi Attestation Works

NSO Album Sync uses the public [**nxapi-znca-api**](https://nxapi-znca-api.fancy.org.uk) service with a registered public client ID (`eJ8TDme0c-Z4czx5SvZabA`) with OAuth scopes `ca:gf ca:er ca:dr`.

- **Zero Emulators / Rooting Needed**: All Coral `f` token generation and encryption/decryption are handled through the public API.
- **Session Caching**: Authenticated Coral tokens are cached for 2 hours in memory, skipping attestation requests on alternating sync intervals.
- **Dynamic Versioning**: NSO client versions are dynamically discovered from `/config` and cached for 6 hours.

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
