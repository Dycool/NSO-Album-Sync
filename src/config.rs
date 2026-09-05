//! Configuration loading/saving with OS credential-store token separation.

use crate::model::AppConfig;
use crate::secure_store::SecureStore;
use atomic_write_file::AtomicWriteFile;
use serde_json::{Map, Value};
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
#[cfg(target_os = "windows")]
use std::process::{Command, Stdio};
use std::sync::{Arc, RwLock};

const SESSION_ACCOUNT: &str = "NintendoAccount";
const LEGACY_RUST_SESSION_ACCOUNT: &str = "NintendoSessionToken";
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
        let parsed = fs::read(&config_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
        let mut config = parsed.as_ref().map(migrate_config).unwrap_or_default();
        let mut needs_rewrite = false;

        if let Some(root) = parsed.as_ref() {
            let discord_version = integer_key(root, "discordPresenceSettingVersion").unwrap_or(0);
            if discord_version < 1 {
                needs_rewrite = true;
            }
            if value_key(root, "discordApplicationId").is_some()
                || value_key(root, "DiscordApplicationId").is_some()
            {
                needs_rewrite = true;
            }

            let stored_token = string_with_legacy_key(root, "sessionToken", "SessionToken", "");
            if is_secure_store_marker(&stored_token) {
                config.set_session_token(read_session_credential());
            } else if stored_token.is_empty() || stored_token == VOLATILE_MARKER {
                config.clear_session();
            } else {
                #[cfg(target_os = "windows")]
                if stored_token.starts_with("dpapi:") {
                    let token = decrypt_legacy_dpapi_token(&stored_token);
                    config.set_session_token(token.clone());
                    if !token.is_empty() && store_session_credential(&token) {
                        needs_rewrite = true;
                    }
                } else {
                    config.set_session_token(stored_token.clone());
                    if store_session_credential(&stored_token) {
                        needs_rewrite = true;
                    }
                }

                #[cfg(not(target_os = "windows"))]
                {
                    config.set_session_token(stored_token.clone());
                    if store_session_credential(&stored_token) {
                        needs_rewrite = true;
                    }
                }
            }
        }

        if config.destination_folder().is_empty() {
            config.set_destination_folder(default_album_folder().to_string_lossy().into_owned());
        }
        config.force_secure_defaults();
        let manager = Self {
            inner: Arc::new(RwLock::new(config)),
            config_path: Arc::new(config_path),
        };
        if needs_rewrite {
            manager.save()?;
        }
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
        erase_session_credentials();
        self.update(|config| {
            config.clear_session();
            config.clear_user_nickname();
        })?;
        Ok(())
    }

    pub fn save(&self) -> anyhow::Result<()> {
        let config = self.snapshot();
        let marker = if config.session_token().is_empty() {
            String::new()
        } else if store_session_credential(config.session_token()) {
            SECURE_MARKER.to_owned()
        } else {
            VOLATILE_MARKER.to_owned()
        };

        let mut json = serde_json::to_value(&config)?;
        if let Some(object) = json.as_object_mut() {
            object.insert("sessionToken".to_owned(), Value::String(marker));
        }
        let serialized = serde_json::to_vec(&json)?;
        let mut file = AtomicWriteFile::open(self.config_path.as_ref())
            .map_err(|_| anyhow::anyhow!("Could not write config.json"))?;
        file.write_all(&serialized)
            .map_err(|_| anyhow::anyhow!("Could not write config.json"))?;
        file.flush()
            .map_err(|_| anyhow::anyhow!("Could not write config.json"))?;
        file.commit()
            .map_err(|_| anyhow::anyhow!("Could not replace config.json"))?;
        make_private_file(self.config_path.as_ref())?;
        Ok(())
    }

    pub fn path(&self) -> &Path { self.config_path.as_ref() }
}

fn migrate_config(root: &Value) -> AppConfig {
    let mut object = Map::new();
    insert_string(&mut object, root, "userNickname", "UserNickname");
    insert_string(&mut object, root, "destinationFolder", "DestinationFolder");
    object.insert(
        "autoSync".to_owned(),
        Value::Bool(bool_with_legacy_key(root, "autoSync", "AutoSyncEnabled", false)),
    );
    object.insert("autoSyncSettingVersion".to_owned(), Value::from(1_u64));
    object.insert(
        "notifications".to_owned(),
        Value::Bool(bool_with_legacy_key(root, "notifications", "NotificationsEnabled", false)),
    );

    let discord_version = integer_key(root, "discordPresenceSettingVersion").unwrap_or(0);
    let discord_enabled = if discord_version >= 1 {
        bool_with_legacy_key(root, "discordPresence", "DiscordPresenceEnabled", false)
    } else {
        false
    };
    object.insert("discordPresence".to_owned(), Value::Bool(discord_enabled));
    object.insert("discordPresenceSettingVersion".to_owned(), Value::from(1_u64));
    object.insert(
        "startOnBoot".to_owned(),
        Value::Bool(bool_with_legacy_key(root, "startOnBoot", "StartOnBoot", false)),
    );
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

fn value_key<'a>(root: &'a Value, key: &str) -> Option<&'a Value> { root.as_object()?.get(key) }
fn string_key(root: &Value, key: &str) -> Option<String> { value_key(root, key)?.as_str().map(ToOwned::to_owned) }
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
    integer_key(root, current).or_else(|| integer_key(root, legacy)).unwrap_or(fallback)
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

fn is_secure_store_marker(value: &str) -> bool {
    matches!(value, SECURE_MARKER | MAC_KEYCHAIN_MARKER | LINUX_SECRET_SERVICE_MARKER)
}

fn read_session_credential() -> String {
    if let Ok(Some(token)) = SecureStore::get(SESSION_ACCOUNT) {
        return token;
    }
    let Ok(Some(token)) = SecureStore::get(LEGACY_RUST_SESSION_ACCOUNT) else {
        return String::new();
    };
    if SecureStore::put(SESSION_ACCOUNT, &token).is_ok() {
        let _ = SecureStore::erase(LEGACY_RUST_SESSION_ACCOUNT);
    }
    token
}

fn store_session_credential(token: &str) -> bool {
    if !SecureStore::available() || SecureStore::put(SESSION_ACCOUNT, token).is_err() {
        return false;
    }
    let _ = SecureStore::erase(LEGACY_RUST_SESSION_ACCOUNT);
    true
}

fn erase_session_credentials() {
    let _ = SecureStore::erase(SESSION_ACCOUNT);
    let _ = SecureStore::erase(LEGACY_RUST_SESSION_ACCOUNT);
}

#[cfg(target_os = "windows")]
fn decrypt_legacy_dpapi_token(value: &str) -> String {
    let Some(encoded) = value.strip_prefix("dpapi:") else {
        return String::new();
    };
    if encoded.is_empty() {
        return String::new();
    }

    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Security
$encoded = [Console]::In.ReadToEnd().Trim()
$cipher = [Convert]::FromBase64String($encoded)
$entropy = [Text.Encoding]::UTF8.GetBytes('NSO_Album_Sync_Salt_9981')
$plain = [Security.Cryptography.ProtectedData]::Unprotect(
    $cipher,
    $entropy,
    [Security.Cryptography.DataProtectionScope]::CurrentUser
)
[Console]::Out.Write([Convert]::ToBase64String($plain))
"#;

    let Ok(mut child) = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
    else {
        return String::new();
    };
    let Some(mut stdin) = child.stdin.take() else {
        return String::new();
    };
    if stdin.write_all(encoded.as_bytes()).is_err() {
        return String::new();
    }
    drop(stdin);
    let Ok(output) = child.wait_with_output() else {
        return String::new();
    };
    if !output.status.success() {
        return String::new();
    }
    let encoded_plain = String::from_utf8_lossy(&output.stdout);
    let Ok(bytes) = crate::util::base64_decode(encoded_plain.trim()) else {
        return String::new();
    };
    String::from_utf8(bytes).unwrap_or_default()
}

pub fn config_directory() -> anyhow::Result<PathBuf> {
    #[cfg(target_os = "windows")]
    {
        let base = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        Ok(base.join("NSOAlbumSync"))
    }
    #[cfg(target_os = "macos")]
    {
        let base = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        Ok(base.join("Library/Application Support/NSOAlbumSync"))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(base) = std::env::var_os("XDG_CONFIG_HOME") {
            Ok(PathBuf::from(base).join("NSOAlbumSync"))
        } else {
            let base = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
            Ok(base.join(".config/NSOAlbumSync"))
        }
    }
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        anyhow::bail!("unsupported platform")
    }
}

pub fn runtime_directory() -> anyhow::Result<PathBuf> {
    let path = config_directory()?.join("runtime");
    make_private_directory(&path)?;
    Ok(path)
}

pub fn default_album_folder() -> PathBuf {
    #[cfg(target_os = "windows")]
    {
        let (pictures, videos) = windows_media_folders();
        let mut candidates = Vec::new();
        if let Some(videos) = videos.as_ref() {
            candidates.extend([
                videos.join("Nintendo Switch 2/Album"),
                videos.join("Nintendo Switch/Album"),
                videos.join("Nintendo Switch 2"),
                videos.join("Nintendo Switch"),
            ]);
        }
        if let Some(pictures) = pictures.as_ref() {
            candidates.extend([
                pictures.join("Nintendo Switch 2/Album"),
                pictures.join("Nintendo Switch/Album"),
                pictures.join("Nintendo Switch 2"),
                pictures.join("Nintendo Switch"),
            ]);
        }
        if let Some(existing) = candidates.into_iter().find(|path| path.exists()) {
            return existing;
        }
        if let Some(pictures) = pictures {
            return pictures.join("Nintendo Switch");
        }
        let profile = std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        return profile.join("Pictures/Nintendo Switch");
    }

    #[cfg(target_os = "macos")]
    {
        let base = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        let candidates = [
            base.join("Movies/Nintendo Switch 2/Album"),
            base.join("Movies/Nintendo Switch/Album"),
            base.join("Movies/Nintendo Switch 2"),
            base.join("Movies/Nintendo Switch"),
            base.join("Pictures/Nintendo Switch 2/Album"),
            base.join("Pictures/Nintendo Switch/Album"),
            base.join("Pictures/Nintendo Switch 2"),
            base.join("Pictures/Nintendo Switch"),
        ];
        return candidates.into_iter().find(|path| path.exists()).unwrap_or_else(|| base.join("Pictures/Nintendo Switch"));
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let base = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
        let candidates = [
            base.join("Videos/Nintendo Switch 2/Album"),
            base.join("Videos/Nintendo Switch/Album"),
            base.join("Videos/Nintendo Switch 2"),
            base.join("Videos/Nintendo Switch"),
            base.join("Pictures/Nintendo Switch 2/Album"),
            base.join("Pictures/Nintendo Switch/Album"),
            base.join("Pictures/Nintendo Switch 2"),
            base.join("Pictures/Nintendo Switch"),
        ];
        return candidates.into_iter().find(|path| path.exists()).unwrap_or_else(|| base.join("Pictures/Nintendo Switch"));
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        PathBuf::from("Pictures/Nintendo Switch")
    }
}

#[cfg(target_os = "windows")]
fn windows_media_folders() -> (Option<PathBuf>, Option<PathBuf>) {
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::Out.WriteLine([Environment]::GetFolderPath([Environment+SpecialFolder]::MyPictures))
[Console]::Out.WriteLine([Environment]::GetFolderPath([Environment+SpecialFolder]::MyVideos))
"#;
    let Ok(output) = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
    else {
        return (None, None);
    };
    if !output.status.success() {
        return (None, None);
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let mut lines = text.lines();
    let pictures = lines.next().map(str::trim).filter(|line| !line.is_empty()).map(PathBuf::from);
    let videos = lines.next().map(str::trim).filter(|line| !line.is_empty()).map(PathBuf::from);
    (pictures, videos)
}

fn make_private_directory(path: &Path) -> anyhow::Result<()> {
    fs::create_dir_all(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o700));
    }
    Ok(())
}

fn make_private_file(_path: &Path) -> anyhow::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let _ = fs::set_permissions(_path, fs::Permissions::from_mode(0o600));
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
