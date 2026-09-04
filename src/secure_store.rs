//! Native OS credential-store adapter using the same identities as the C++ build.

use keyring::v1::Entry as V1Entry;
use keyring_core::{Entry, Error};
#[cfg(any(target_os = "windows", target_os = "linux"))]
use std::collections::HashMap;

const LEGACY_RUST_SERVICE: &str = "NSO Album Sync";
#[cfg(any(target_os = "macos", target_os = "linux"))]
const APPLE_SERVICE: &str = "org.nsoalbumsync.session-token";
#[cfg(target_os = "linux")]
const LINUX_APPLICATION: &str = "NsoAlbumSync";
#[cfg(target_os = "linux")]
const LINUX_LEGACY_SESSION_KEY: &str = "session_token";
#[cfg(target_os = "linux")]
const NINTENDO_ACCOUNT: &str = "NintendoAccount";

pub struct SecureStore;

impl SecureStore {
    pub fn available() -> bool {
        V1Entry::store_status().is_ok()
    }

    pub fn put(account: &str, secret: &str) -> anyhow::Result<()> {
        anyhow::ensure!(Self::available(), "OS credential store is unavailable");
        put_native(account, secret)?;
        erase_legacy_rust(account);
        Ok(())
    }

    pub fn get(account: &str) -> anyhow::Result<Option<String>> {
        if !Self::available() {
            return Ok(None);
        }
        if let Some(secret) = get_native(account)? {
            erase_legacy_rust(account);
            return Ok(Some(secret));
        }

        let Some(secret) = get_legacy_rust(account)? else {
            return Ok(None);
        };
        if put_native(account, &secret).is_ok() {
            erase_legacy_rust(account);
        }
        Ok(Some(secret))
    }

    pub fn erase(account: &str) -> anyhow::Result<()> {
        if !Self::available() {
            return Ok(());
        }
        erase_native(account)?;
        erase_legacy_rust(account);
        Ok(())
    }
}

fn get_password(entry: &Entry) -> anyhow::Result<Option<String>> {
    match entry.get_password() {
        Ok(secret) => Ok(Some(secret)),
        Err(Error::NoEntry) => Ok(None),
        Err(error) => Err(error.into()),
    }
}

fn erase_entry(entry: &Entry) -> anyhow::Result<()> {
    match entry.delete_credential() {
        Ok(()) | Err(Error::NoEntry) => Ok(()),
        Err(error) => Err(error.into()),
    }
}

#[cfg(target_os = "windows")]
fn windows_entry(account: &str) -> anyhow::Result<Entry> {
    let target = format!("Dycool.NSOAlbumSync/{account}");
    let modifiers = HashMap::from([
        ("target", target.as_str()),
        ("persistence", "LocalMachine"),
    ]);
    Ok(Entry::new_with_modifiers(
        "NSO Album Sync",
        "Nintendo Account",
        &modifiers,
    )?)
}

#[cfg(target_os = "macos")]
fn apple_entry(account: &str) -> anyhow::Result<Entry> {
    Ok(Entry::new(APPLE_SERVICE, account)?)
}

#[cfg(target_os = "linux")]
fn linux_entries_for_key(key: &str) -> anyhow::Result<Vec<Entry>> {
    let spec = HashMap::from([
        ("application", LINUX_APPLICATION),
        ("key", key),
    ]);
    Ok(Entry::search(&spec)?)
}

#[cfg(target_os = "linux")]
fn linux_entries(account: &str) -> anyhow::Result<Vec<Entry>> {
    linux_entries_for_key(account)
}

#[cfg(target_os = "linux")]
fn linux_create_entry(account: &str, secret: &str) -> anyhow::Result<()> {
    let entry = Entry::new(APPLE_SERVICE, account)?;
    entry.set_password(secret)?;
    let attributes = HashMap::from([
        ("application", LINUX_APPLICATION),
        ("key", account),
    ]);
    if let Err(error) = entry.update_attributes(&attributes) {
        let _ = entry.delete_credential();
        return Err(error.into());
    }
    Ok(())
}

#[cfg(target_os = "windows")]
fn put_native(account: &str, secret: &str) -> anyhow::Result<()> {
    windows_entry(account)?.set_password(secret)?;
    Ok(())
}

#[cfg(target_os = "macos")]
fn put_native(account: &str, secret: &str) -> anyhow::Result<()> {
    apple_entry(account)?.set_password(secret)?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn put_native(account: &str, secret: &str) -> anyhow::Result<()> {
    let entries = linux_entries(account)?;
    if let Some(entry) = entries.first() {
        entry.set_password(secret)?;
        for duplicate in entries.iter().skip(1) {
            let _ = duplicate.delete_credential();
        }
    } else {
        linux_create_entry(account, secret)?;
    }
    if account == NINTENDO_ACCOUNT {
        for legacy in linux_entries_for_key(LINUX_LEGACY_SESSION_KEY)? {
            let _ = legacy.delete_credential();
        }
    }
    Ok(())
}

#[cfg(target_os = "windows")]
fn get_native(account: &str) -> anyhow::Result<Option<String>> {
    get_password(&windows_entry(account)?)
}

#[cfg(target_os = "macos")]
fn get_native(account: &str) -> anyhow::Result<Option<String>> {
    get_password(&apple_entry(account)?)
}

#[cfg(target_os = "linux")]
fn get_native(account: &str) -> anyhow::Result<Option<String>> {
    let entries = linux_entries(account)?;
    if let Some(entry) = entries.first() {
        return get_password(entry);
    }
    if account == NINTENDO_ACCOUNT {
        let legacy = linux_entries_for_key(LINUX_LEGACY_SESSION_KEY)?;
        if let Some(entry) = legacy.first() {
            return get_password(entry);
        }
    }
    Ok(None)
}

#[cfg(target_os = "windows")]
fn erase_native(account: &str) -> anyhow::Result<()> {
    erase_entry(&windows_entry(account)?)
}

#[cfg(target_os = "macos")]
fn erase_native(account: &str) -> anyhow::Result<()> {
    erase_entry(&apple_entry(account)?)
}

#[cfg(target_os = "linux")]
fn erase_native(account: &str) -> anyhow::Result<()> {
    for entry in linux_entries(account)? {
        erase_entry(&entry)?;
    }
    if account == NINTENDO_ACCOUNT {
        for entry in linux_entries_for_key(LINUX_LEGACY_SESSION_KEY)? {
            erase_entry(&entry)?;
        }
    }
    Ok(())
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn put_native(_account: &str, _secret: &str) -> anyhow::Result<()> {
    anyhow::bail!("OS credential store is unavailable")
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn get_native(_account: &str) -> anyhow::Result<Option<String>> {
    Ok(None)
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn erase_native(_account: &str) -> anyhow::Result<()> {
    Ok(())
}

fn legacy_rust_entry(account: &str) -> anyhow::Result<V1Entry> {
    Ok(V1Entry::new(LEGACY_RUST_SERVICE, account)?)
}

fn get_legacy_rust(account: &str) -> anyhow::Result<Option<String>> {
    let entry = legacy_rust_entry(account)?;
    match entry.get_password() {
        Ok(secret) => Ok(Some(secret)),
        Err(keyring::v1::Error::NoEntry) => Ok(None),
        Err(error) => Err(error.into()),
    }
}

fn erase_legacy_rust(account: &str) {
    if let Ok(entry) = legacy_rust_entry(account) {
        match entry.delete_credential() {
            Ok(()) | Err(keyring::v1::Error::NoEntry) | Err(_) => {}
        }
    }
}
