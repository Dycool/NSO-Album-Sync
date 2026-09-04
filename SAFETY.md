# Cargo safety boundary

The Rust port deliberately keeps build and safety policy in a small set of owner-reviewed files:

- `Cargo.toml`
- `rust-toolchain.toml`
- `src/lib.rs`
- `src/main.rs`
- `scripts/check_rust_safety.py`

The compiler-level `#![forbid(unsafe_code)]` attributes and Cargo lint configuration are authoritative. The safety script is defense in depth and CI validation; it is not a substitute for the compiler ban.

`Cargo.toml` sets `build = false`, so Cargo will not auto-discover or execute a `build.rs`. The safety gate additionally rejects any `build.rs` that appears in the repository.
