<p align="center">
  <img src="icon.svg" alt="NSO Album Sync icon" width="128" height="128">
</p>

# NSO Album Sync

**Automatically sync your Nintendo Switch Online album to your computer.**

🖼️ **Automatic album sync** — Downloads new screenshots and videos in the same `Album/<Game Name>/` layout used by Nintendo Switch USB transfers.

🎮 **Discord Rich Presence** — Optionally shows `Playing <Game Name>` with Nintendo Switch / Nintendo Switch 2 underneath and the game's artwork.

🌍 **Localized game folders** — Reuses existing game folders across different languages instead of creating duplicates.

🕒 **Original timestamps** — Keeps the original capture time on downloaded screenshots and videos.

🔐 **Secure sign-in** — Nintendo and Coral credentials are stored using the operating system credential store when available.

🖥️ **Cross-platform** — Native tray/menu-bar app for Windows, macOS and Linux.

> **Pre-compiled Binaries Available!**
> Download NSO Album Sync for Windows, macOS and Linux from the **[Releases](https://github.com/Dycool/NSO-Album-Sync/releases)** page.

---

## 🚀 Quick Start

1. Download the build for your operating system from **Releases**.
2. Open NSO Album Sync and sign in with your Nintendo Account.
3. Choose your album folder if you do not want the default Pictures folder.
4. Leave the app running in the tray/menu bar. Auto-sync runs every 60 minutes by default.

Discord Rich Presence is optional and disabled by default.

---

## 🔨 Building

Requires **CMake 3.20+**, **Python 3** and a **C++20** compiler. Discord gates Social SDK downloads behind the Developer Portal, so provide the official SDK archive locally or set `DISCORD_SOCIAL_SDK_URL` to its direct official HTTPS download URL. No Discord secret, token or OAuth setup is used by the app.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Windows builds are unsigned. macOS builds use local/ad-hoc codesigning and do not require an Apple Developer membership.

---

## 🔐 Nintendo Account & nxapi

NSO Album Sync uses the third-party [`nxapi-znca-api`](https://github.com/samuelthomas2774/nxapi-znca-api) service for Nintendo Switch Online request attestation and Coral request/response encryption.

The app shows a disclosure before sign-in because Nintendo Account/Coral authentication data and Coral API traffic are processed by that service. nxapi authentication tokens are kept in memory only, while reusable Nintendo/Coral credentials are stored in the operating system credential store when available.

---

## 📄 License

Licensed under the [MIT License](LICENSE).
