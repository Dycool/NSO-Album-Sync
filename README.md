<p align="center">
  <img src="icon.png" alt="Icon" width="128" height="128">
</p>

# NSO Album Sync

**Silently and automatically sync your Nintendo Switch & Switch 2 album screenshots and videos directly to your PC in the background.**

🔄 **Hourly Background Auto-Sync** - Silently checks your Nintendo Switch Online album every hour and downloads new screenshots and gameplay videos without interrupting you.

📁 **Official Switch USB Structure** - Saves captures matching the exact directory hierarchy used by Nintendo Switch during USB PC transfers (`Album/<Game Name>/YYYYMMDDHHMMSS00_c.ext`).

💾 **Smart Pre-Existing USB Album Detection** - Point the app to your existing album folder and it will immediately detect every photo and video you already backed up, downloading 0 redundant files.

🆔 **Dynamic Title ID ⟷ Local Folder Learning** - Automatically pairs universal Nintendo Title IDs with your existing local folder names (including Portuguese, Japanese, or custom names) so new captures always route into your existing directories without creating duplicates.

🕒 **Preserves Original Capture Timestamps** - Automatically sets the local filesystem creation and modified timestamps to match the exact hardware RTC capture moment on your console.

🔄 **Automatic Local File Restoration** - If you delete a local capture that is still present in your Nintendo cloud album (recent 100 captures), the next sync automatically restores and re-downloads it.

🔒 **Encrypted Credential Storage** - Session tokens are encrypted on disk using **Windows DPAPI** (`ProtectedData` with `CurrentUser` scope) on Windows, and locked to `0600` user permissions on macOS and Linux.

🖥️ **Zero-UI System Tray App** - Runs silently in your taskbar notification area with a native context menu:
* **Sync Now** - Trigger an immediate album sync on demand.
* **Auto-Sync (Hourly)** - Toggle automatic background sync.
* **Select Folder...** - Change your album destination.
* **Open Album Folder** - Shortcut directly to your album in Windows Explorer / Finder.
* **Start on Boot** - Automatically launch on computer startup.
* **Sign In / Sign Out** - Connect or switch your Nintendo Account.

🪶 **Ultra-Lightweight** - Fast single-file executable (~290 KB) with minimal memory footprint (~25 MB RAM).

🛡️ **Public `nxapi-znca-api` Integration** - Zero Android emulators or rooting needed. Strictly follows maintainer guidelines with 2-hour Coral session caching, dynamic version discovery, and combined `/f` attestation.

> **Pre-compiled Binaries Available!**
> You can download ready-to-use binaries for Windows, macOS (codesigned), and Linux directly from the **[Releases](https://github.com/Dycool/nso-album-sync/releases)** page.

---

## 🚀 Quick Start (Pre-compiled)

**1. 🪟 Windows:**
* Download `nso-album-sync.exe` from [**Releases](https://github.com/Dycool/nso-album-sync/releases)**.
* Double-click `nso-album-sync.exe` to launch into your System Tray.
* Click **"🌐 1. Open Nintendo Sign-In Page"** in the one-time sign-in window.
* Log into your Nintendo Account in your browser, right-click (or copy link) on the **"Select this person"** button, paste the link into the app, and click **"✅ Sign In & Connect"**.
* *(Optional)* Right-click the tray icon and toggle **"🚀 Start on Boot"**.

**2. 🍎 macOS & 🐧 Linux:**
* Download the macOS (`tar.gz`) or Linux binary from [**Releases](https://github.com/Dycool/nso-album-sync/releases)**.
* Extract and run `./nso-album-sync` (macOS binaries are pre-codesigned with ad-hoc signatures).

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
