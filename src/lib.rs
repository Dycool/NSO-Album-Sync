#![forbid(unsafe_code)]

//! Safe Rust implementation of NSO Album Sync.
//!
//! The desktop integrations are feature-gated so Miri can execute the pure core
//! without loading platform UI, credential-store, IPC, or networking backends.

pub mod game_aliases;
pub mod model;
pub mod util;
pub mod zelda_regions;

#[cfg(test)]
mod cpp_config_parity;

#[cfg(feature = "desktop")]
pub mod app;
#[cfg(feature = "desktop")]
pub mod auth_callback;
#[cfg(feature = "desktop")]
pub mod config;
#[cfg(feature = "desktop")]
pub mod coral;
#[cfg(feature = "desktop")]
pub mod discord;
#[cfg(feature = "desktop")]
pub mod game_services;
#[cfg(feature = "desktop")]
pub mod http;
#[cfg(feature = "desktop")]
pub mod nintendo_auth;
#[cfg(feature = "desktop")]
pub mod nxapi;
#[cfg(feature = "desktop")]
pub mod platform;
#[cfg(feature = "desktop")]
pub mod secure_store;
#[cfg(feature = "desktop")]
pub mod single_instance;
#[cfg(feature = "desktop")]
pub mod splatnet;
#[cfg(feature = "desktop")]
pub mod sse;
#[cfg(feature = "desktop")]
pub mod sync;
#[cfg(feature = "desktop")]
pub mod zelda_notes;
