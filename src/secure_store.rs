//! Native OS credential-store adapter using the same identities as the C++ build.

use keyring::v1::Entry as V1Entry;
use keyring_core::{Entry, Error};
#[cfg(any(target_os = "windows", target_os = "linux"))]
use std::collections::HashMap;
#[cfg(target_os = "macos")]
use std::fs::{self, File};
#[cfg(target_os = "macos")]
use std::io::Read as _;
#[cfg(target_os = "macos")]
use std::os::unix::fs::MetadataExt as _;
#[cfg(target_os = "macos")]
use std::path::PathBuf;
#[cfg(target_os = "macos")]
use std::process::Command;
#[cfg(target_os = "macos")]
use std::sync::OnceLock;

const LEGACY_RUST_SERVICE: &str = "NSO Album Sync";
#[cfg(any(target_os = "macos", target_os = "linux"))]
const APPLE_SERVICE: &str = "org.nsoalbumsync.session-token";
#[cfg(target_os = "macos")]
const MAX_LEGACY_SECRET_BYTES: u64 = 64 * 1024;
#[cfg(target_os = "linux")]
const LINUX_APPLICATION: &str = "NsoAlbumSync";
#[cfg(target_os = "linux")]
const LINUX_LEGACY_SESSION_KEY: &str = "session_token";
#[cfg(target_os = "linux")]
const NINTENDO_ACCOUNT: &str = "NintendoAccount";

pub struct SecureStore;

impl SecureStore {
    pub fn available() -> bool {
        #[cfg(target_os = "macos")]
        {
            migrate_known_legacy_credentials();
            true
        }
        #[cfg(not(target_os = "macos"))]
        {
            V1Entry::store_status().is_ok()
        }
    }

    pub fn put(account: &str, secret: &str) -> anyhow::Result<()> {
        anyhow::ensure!(Self::available(), "OS credential store is unavailable");
        #[cfg(target_os = "macos")]
        {
            let stored = put_native(account, secret);
            remove_legacy_credential(account);
            erase_legacy_rust(account);
            stored?;
            Ok(())
        }
        #[cfg(not(target_os = "macos"))]
        {
            put_native(account, secret)?;
            erase_legacy_rust(account);
            Ok(())
        }
    }

    pub fn get(account: &str) -> anyhow::Result<Option<String>> {
        if !Self::available() {
            return Ok(None);
        }
        #[cfg(target_os = "macos")]
        {
            if let Some(secret) = get_native(account).ok().flatten() {
                remove_legacy_credential(account);
                erase_legacy_rust(account);
                return Ok(Some(secret));
            }
            if let Some(secret) = migrate_legacy_credential(account) {
                erase_legacy_rust(account);
                return Ok(Some(secret));
            }
        }
        #[cfg(not(target_os = "macos"))]
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
        #[cfg(target_os = "macos")]
        {
            let _ = erase_native(account);
            remove_legacy_credential(account);
            erase_legacy_rust(account);
            Ok(())
        }
        #[cfg(not(target_os = "macos"))]
        {
            erase_native(account)?;
            erase_legacy_rust(account);
            Ok(())
        }
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

#[cfg(target_os = "macos")]
fn legacy_credentials_directory() -> PathBuf {
    let home = std::env::var_os("HOME").unwrap_or_else(|| ".".into());
    PathBuf::from(home).join("Library/Application Support/NSOAlbumSync/credentials")
}

#[cfg(target_os = "macos")]
fn safe_account_name(account: &str) -> String {
    let safe = account
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_') {
                char::from(byte)
            } else {
                '_'
            }
        })
        .collect::<String>();
    if safe.is_empty() { "credential".to_owned() } else { safe }
}

#[cfg(target_os = "macos")]
fn legacy_credential_path(account: &str) -> PathBuf {
    legacy_credentials_directory().join(format!("{}.dat", safe_account_name(account)))
}

#[cfg(target_os = "macos")]
fn remove_legacy_credential(account: &str) {
    let _ = fs::remove_file(legacy_credential_path(account));
    let _ = fs::remove_dir(legacy_credentials_directory());
}

#[cfg(target_os = "macos")]
fn current_uid() -> Option<u32> {
    let output = Command::new("/usr/bin/id").arg("-u").output().ok()?;
    if !output.status.success() {
        return None;
    }
    String::from_utf8(output.stdout).ok()?.trim().parse().ok()
}

#[cfg(target_os = "macos")]
fn read_legacy_credential(account: &str) -> Option<String> {
    let path = legacy_credential_path(account);
    let path_metadata = fs::symlink_metadata(&path).ok()?;
    if path_metadata.file_type().is_symlink() || !path_metadata.is_file() {
        return None;
    }
    let mut file = File::open(&path).ok()?;
    let metadata = file.metadata().ok()?;
    let uid = current_uid()?;
    if !metadata.is_file()
        || metadata.uid() != uid
        || metadata.mode() & 0o077 != 0
        || metadata.len() == 0
        || metadata.len() > MAX_LEGACY_SECRET_BYTES
    {
        return None;
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes).ok()?;
    if bytes.len() as u64 != metadata.len() {
        return None;
    }
    String::from_utf8(bytes).ok()
}

#[cfg(target_os = "macos")]
fn migrate_legacy_credential(account: &str) -> Option<String> {
    let legacy = read_legacy_credential(account);
    let Some(secret) = legacy else {
        remove_legacy_credential(account);
        return None;
    };
    let stored = put_native(account, &secret).is_ok();
    remove_legacy_credential(account);
    stored.then_some(secret)
}

#[cfg(target_os = "macos")]
fn migrate_known_legacy_credentials() {
    static MIGRATED: OnceLock<()> = OnceLock::new();
    MIGRATED.get_or_init(|| {
        for account in ["NintendoAccount", "CoralCredential"] {
            if get_native(account).ok().flatten().is_some() {
                remove_legacy_credential(account);
            } else {
                let _ = migrate_legacy_credential(account);
            }
        }
    });
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
