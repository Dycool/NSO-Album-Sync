<p align="center">
  <img src="icon.svg" alt="NSO Album Sync icon" width="128" height="128">
</p>

# NSO Album Sync

**Automatically sync your Nintendo Switch Online album to your computer.**

This branch is the native **safe Rust** implementation of NSO Album Sync. The application keeps the original album synchronization, Nintendo Account sign-in, Coral/nxapi integration, optional Discord Rich Presence, SplatNet/NookLink enrichment, Zelda Notes live location enrichment, startup integration, and desktop tray workflow without using C++ or application-owned `unsafe` Rust.

## Features

- Automatic screenshot/video synchronization into Nintendo-compatible `Album/<Game Name>/` folders.
- Existing localized/synonym game folders are reused rather than duplicated.
- Original capture timestamps are preserved.
- Nintendo Account OAuth uses PKCE and validates the callback state.
- Nintendo session and Coral service credentials are stored in the operating-system credential store when available; config files never fall back to plaintext secrets.
- Media downloads are restricted to bounded public HTTPS URLs, do not follow redirects, and are capped at 256 MiB.
- Optional Discord Rich Presence through Discord IPC, with Splatoon 3, Splatoon 2, Animal Crossing and Zelda Notes enrichment.
- Native Windows, macOS and Linux tray integration.

## Build

The repository pins Rust **1.98.1** in `rust-toolchain.toml`.

```sh
cargo build --release
```

On Debian/Ubuntu, the tray backend also needs:

```sh
sudo apt install libgtk-3-dev libxdo-dev libayatana-appindicator3-dev pkg-config
```

Run tests and the compiler safety gate with:

```sh
python scripts/check_rust_safety.py
cargo check --all-targets
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
```

The pure safe core is deliberately separable from desktop/network integrations and is also tested with Miri:

```sh
cargo +nightly miri test --lib --no-default-features
```

## Safe-Rust boundary

The safety boundary is enforced by the repository rather than depending on coding instructions:

- `src/lib.rs` and `src/main.rs` contain `#![forbid(unsafe_code)]`.
- `Cargo.toml` also sets `unsafe_code = "forbid"` and forbids the requested Clippy unsafe/transmute lints.
- `package.build = false` disables Cargo build-script auto-discovery, and `build.rs` is rejected by the safety gate.
- The safety gate rejects crate-level `#![allow(...)]` / `#![warn(...)]` overrides, application FFI, raw pointer types and explicit unsafe constructs.
- Application model/config fields are private and are changed through invariant-preserving methods.
- CI runs `cargo clippy --all-targets -- -D warnings` and Miri.

## Nintendo Account & nxapi

NSO Album Sync uses the third-party [`nxapi-znca-api`](https://github.com/samuelthomas2774/nxapi-znca-api) service for Nintendo Switch Online request attestation and Coral request/response encryption. Nintendo and nxapi short-lived authentication material is kept in memory; reusable credentials are persisted only through the OS credential store.

## License

Licensed under the [MIT License](LICENSE). Third-party notices are available in [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt).
