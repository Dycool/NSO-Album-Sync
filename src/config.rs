//! Configuration loading/saving with OS credential-store token separation.

use crate::model::AppConfig;
use crate::secure_store::SecureStore;
use atomic_write_file::AtomicWriteFile;
use serde_json::{Map, Value};
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

const SESSION_ACCOUNT: &str = "NintendoSessionToken";
const SECURE_MARKER: &str = "secure:v1";
const MAC_KEYCHAIN_MARKER: &str = "keychain:v1";
const LINUX_SECRET_SERVICE_MARKER: &str = "secret-service:v1";
const VOLATILE_MARKER: &str = "volatile:v1";

#[derive(Clone)]
pub struct ConfigManager {
    inner: Arc<RwLock<AppConfig>>,
    config_path: Arc<PathBuf>,
}

impl ConfigManager {
    pub fn load() -> anyhow::Result<Self> {
        let directory = config_directory()?;
        make_private_directory(&directory)?;
        let config_path = directory.join("config.json");

        // Match the C++ app's fail-soft migration behavior: malformed, partial,
        // or unknown configuration must never prevent the tray app from starting.
        let root = fs::read(&config_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
            .unwrap_or_else(|| Value::Object(Map::new()));
        let mut config = migrate_config(&root);
        let stored_token = string_with_legacy_key(&root, "sessionToken", "SessionToken", "");

        match stored_token.as_str() {
            SECURE_MARKER | MAC_KEYCHAIN_MARKER | LINUX_SECRET_SERVICE_MARKER => {
                let token = SecureStore::get(SESSION_ACCOUNT)
                    .ok()
                    .flatten()
                    .unwrap_or_default();
                let nickname = config.user_nickname().to_owned();
                config.set_session(token, nickname);
            }
            "" | VOLATILE_MARKER => config.clear_session(),
            value if value.starts_with("dpapi:") => {
                // The previous Windows build could leave a DPAPI migration blob.
                // Safe Rust deliberately has no platform FFI escape hatch here;
                // fail closed and require one fresh browser sign-in instead of
                // risking plaintext persistence or shelling secret material out.
                config.clear_session();
            }
            legacy_token => {
                let nickname = config.user_nickname().to_owned();
                config.set_session(legacy_token.to_owned(), nickname);
                if SecureStore::available() {
                    let _ = SecureStore::put(SESSION_ACCOUNT, legacy_token);
                }
            }
        }

        if config.destination_folder().trim().is_empty() {
            config.set_destination_folder(default_album_folder().to_string_lossy().into_owned());
        }
        config.force_secure_defaults();

        let manager = Self {
            inner: Arc::new(RwLock::new(config)),
            config_path: Arc::new(config_path),
        };
        // Rewrites legacy names, old Discord consent defaults and any plaintext
        // session token to the current marker-only storage format immediately.
        manager.save()?;
        Ok(manager)
    }

    pub fn snapshot(&self) -> AppConfig {
        self.inner.read().unwrap_or_else(|poisoned| poisoned.into_inner()).clone()
    }

    pub fn update<F>(&self, update: F) -> anyhow::Result<AppConfig>
    where
        F: FnOnce(&mut AppConfig),
    {
        {
            let mut guard = self.inner.write().unwrap_or_else(|poisoned| poisoned.into_inner());
            update(&mut guard);
            guard.force_secure_defaults();
        }
        self.save()?;
        Ok(self.snapshot())
    }

    pub fn clear_session(&self) -> anyhow::Result<()> {
        let _ = SecureStore::erase(SESSION_ACCOUNT);
        self.update(AppConfig::clear_session)?;
        Ok(())
    }

    pub fn save(&self) -> anyhow::Result<()> {
        let config = self.snapshot();
        let mut json = serde_json::to_value(&config)?;
        let marker = if config.session_token().is_empty() {
            String::new()
        } else if SecureStore::available()
            && SecureStore::put(SESSION_ACCOUNT, config.session_token()).is_ok()
        {
            SECURE_MARKER.to_owned()
        } else {
            // Keep the valid token process-local rather than degrading to a
            // plaintext config file when the platform keychain is unavailable.
            VOLATILE_MARKER.to_owned()
        };
        if let Some(object) = json.as_object_mut() {
            object.insert("sessionToken".to_owned(), Value::String(marker));
        }
        let serialized = serde_json::to_vec_pretty(&json)?;
        let mut file = AtomicWriteFile::open(self.config_path.as_ref())?;
        file.write_all(&serialized)?;
        file.flush()?;
        file.commit()?;
        make_private_file(self.config_path.as_ref())?;
        Ok(())
    }

    pub fn path(&self) -> &Path { self.config_path.as_ref() }
}

fn migrate_config(root: &Value) -> AppConfig {
    let mut object = Map::new();
    insert_string(&mut object, root, "userNickname", "UserNickname");
    insert_string(&mut object, root, "destinationFolder", "DestinationFolder");
    insert_bool(&mut object, root, "autoSync", "AutoSyncEnabled");
    object.insert("autoSyncSettingVersion".to_owned(), Value::from(1_u64));
    insert_bool(&mut object, root, "notifications", "NotificationsEnabled");

    let discord_version = integer_key(root, "discordPresenceSettingVersion")
        .unwrap_or(0);
    let discord_enabled = if discord_version >= 1 {
        bool_with_legacy_key(root, "discordPresence", "DiscordPresenceEnabled", false)
    } else {
        // Older builds enabled RPC by default. Requiring an explicit opt-in
        // after migration preserves the C++ consent boundary.
        false
    };
    object.insert("discordPresence".to_owned(), Value::Bool(discord_enabled));
    object.insert("discordPresenceSettingVersion".to_owned(), Value::from(1_u64));

    insert_bool(&mut object, root, "startOnBoot", "StartOnBoot");
    let interval = integer_with_legacy_key(root, "syncIntervalMinutes", "SyncIntervalMinutes", 60)
        .clamp(1, i64::from(i32::MAX));
    object.insert("syncIntervalMinutes".to_owned(), Value::from(interval));

    let stored_last_sync = string_with_legacy_key(root, "lastSync", "LastSyncTime", "Never");
    object.insert("lastSync".to_owned(), Value::String(display_last_sync(&stored_last_sync)));
    insert_string(&mut object, root, "proxyUrl", "ProxyUrl");
    insert_string(&mut object, root, "nxapiAuthClientId", "NxapiAuthClientId");

    serde_json::from_value(Value::Object(object)).unwrap_or_default()
}

fn insert_string(output: &mut Map<String, Value>, root: &Value, current: &str, legacy: &str) {
    let value = string_with_legacy_key(root, current, legacy, "");
    if !value.is_empty() {
        output.insert(current.to_owned(), Value::String(value));
    }
}

fn insert_bool(output: &mut Map<String, Value>, root: &Value, current: &str, legacy: &str) {
    if value_key(root, current).is_some_and(Value::is_boolean)
        || value_key(root, legacy).is_some_and(Value::is_boolean)
    {
        output.insert(
            current.to_owned(),
            Value::Bool(bool_with_legacy_key(root, current, legacy, false)),
        );
    }
}

fn value_key<'a>(root: &'a Value, key: &str) -> Option<&'a Value> {
    root.as_object()?.get(key)
}

fn string_key(root: &Value, key: &str) -> Option<String> {
    value_key(root, key)?.as_str().map(ToOwned::to_owned)
}

fn integer_key(root: &Value, key: &str) -> Option<i64> {
    let value = value_key(root, key)?;
    value.as_i64().or_else(|| value.as_u64().and_then(|number| i64::try_from(number).ok()))
}

fn string_with_legacy_key(root: &Value, current: &str, legacy: &str, fallback: &str) -> String {
    string_key(root, current)
        .filter(|value| !value.is_empty())
        .or_else(|| string_key(root, legacy))
        .unwrap_or_else(|| fallback.to_owned())
}

fn bool_with_legacy_key(root: &Value, current: &str, legacy: &str, fallback: bool) -> bool {
    value_key(root, current)
        .and_then(Value::as_bool)
        .or_else(|| value_key(root, legacy).and_then(Value::as_bool))
        .unwrap_or(fallback)
}

fn integer_with_legacy_key(root: &Value, current: &str, legacy: &str, fallback: i64) -> i64 {
    integer_key(root, current)
        .or_else(|| integer_key(root, legacy))
        .unwrap_or(fallback)
}

fn display_last_sync(stored: &str) -> String {
    if stored.is_empty() {
        return "Never".to_owned();
    }
    let bytes = stored.as_bytes();
    if bytes.len() >= 16
        && bytes.get(4) == Some(&b'-')
        && bytes.get(7) == Some(&b'-')
        && matches!(bytes.get(10), Some(b'T' | b' '))
    {
        return format!("{} ({})", &stored[11..16], &stored[..10]);
    }
    stored.to_owned()
}

pub fn config_directory() -> anyhow::Result<PathBuf> {
    #[cfg(target_os = "windows")]
    {
        let base = std::env::var_os("APPDATA")
            .ok_or_else(|| anyhow::anyhow!("APPDATA is unavailable"))?;
        return Ok(PathBuf::from(base).join("NSOAlbumSync"));
    }
    #[cfg(target_os = "macos")]
    {
        let home = std::env::var_os("HOME")
            .ok_or_else(|| anyhow::anyhow!("HOME is unavailable"))?;
        return Ok(PathBuf::from(home).join("Library/Application Support/NSOAlbumSync"));
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(base) = std::env::var_os("XDG_CONFIG_HOME") {
            return Ok(PathBuf::from(base).join("NSOAlbumSync"));
        }
        let home = std::env::var_os("HOME")
            .ok_or_else(|| anyhow::anyhow!("HOME is unavailable"))?;
        return Ok(PathBuf::from(home).join(".config/NSOAlbumSync"));
    }
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    anyhow::bail!("unsupported platform");
}

pub fn runtime_directory() -> anyhow::Result<PathBuf> {
    let path = config_directory()?.join("runtime");
    make_private_directory(&path)?;
    Ok(path)
}

pub fn default_album_folder() -> PathBuf {
    let home = std::env::var_os(if cfg!(target_os = "windows") { "USERPROFILE" } else { "HOME" })
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    let candidates = [
        home.join("Videos/Nintendo Switch 2/Album"),
        home.join("Pictures/Nintendo Switch 2/Album"),
        home.join("Videos/Nintendo Switch/Album"),
        home.join("Pictures/Nintendo Switch/Album"),
    ];
    candidates
        .into_iter()
        .find(|path| path.is_dir())
        .unwrap_or_else(|| home.join("Pictures/Nintendo Switch"))
}

fn make_private_directory(path: &Path) -> anyhow::Result<()> {
    fs::create_dir_all(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

fn make_private_file(path: &Path) -> anyhow::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{display_last_sync, migrate_config};
    use serde_json::json;

    #[test]
    fn migrates_legacy_pascal_case_config() {
        let config = migrate_config(&json!({
            "UserNickname": "Link",
            "DestinationFolder": "/tmp/Album",
            "AutoSyncEnabled": true,
            "NotificationsEnabled": true,
            "DiscordPresenceEnabled": true,
            "SyncIntervalMinutes": 30,
            "LastSyncTime": "2026-09-04T17:45:00Z"
        }));
        assert_eq!(config.user_nickname(), "Link");
        assert_eq!(config.destination_folder(), "/tmp/Album");
        assert!(config.auto_sync());
        assert!(config.notifications());
        assert!(!config.discord_presence(), "legacy default must require fresh consent");
        assert_eq!(config.sync_interval_minutes(), 30);
        assert_eq!(config.last_sync(), "17:45 (2026-09-04)");
    }

    #[test]
    fn displays_iso_last_sync_like_cpp_build() {
        assert_eq!(display_last_sync("2026-09-04T17:45:00Z"), "17:45 (2026-09-04)");
        assert_eq!(display_last_sync("Never"), "Never");
    }
}
