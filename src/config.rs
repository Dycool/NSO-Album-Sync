//! Configuration loading/saving with OS credential-store token separation.

use crate::model::AppConfig;
use crate::secure_store::SecureStore;
use anyhow::Context as _;
use atomic_write_file::AtomicWriteFile;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

const SESSION_ACCOUNT: &str = "NintendoSessionToken";
const SECURE_MARKER: &str = "secure:v1";
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
        let mut config = if config_path.exists() {
            let bytes = fs::read(&config_path).context("read config.json")?;
            serde_json::from_slice::<AppConfig>(&bytes).context("parse config.json")?
        } else {
            AppConfig::default()
        };

        let stored_token = config.session_token().to_owned();
        match stored_token.as_str() {
            SECURE_MARKER => {
                let token = SecureStore::get(SESSION_ACCOUNT)?.unwrap_or_default();
                config.set_session(token, config.user_nickname().to_owned());
            }
            VOLATILE_MARKER => config.clear_session(),
            "" => {}
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
        } else if SecureStore::available() && SecureStore::put(SESSION_ACCOUNT, config.session_token()).is_ok() {
            SECURE_MARKER.to_owned()
        } else {
            VOLATILE_MARKER.to_owned()
        };
        if let Some(object) = json.as_object_mut() {
            object.insert("sessionToken".to_owned(), serde_json::Value::String(marker));
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

pub fn config_directory() -> anyhow::Result<PathBuf> {
    #[cfg(target_os = "windows")]
    {
        let base = std::env::var_os("APPDATA").context("APPDATA is unavailable")?;
        return Ok(PathBuf::from(base).join("NSOAlbumSync"));
    }
    #[cfg(target_os = "macos")]
    {
        let home = std::env::var_os("HOME").context("HOME is unavailable")?;
        return Ok(PathBuf::from(home).join("Library/Application Support/NSOAlbumSync"));
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(base) = std::env::var_os("XDG_CONFIG_HOME") {
            return Ok(PathBuf::from(base).join("NSOAlbumSync"));
        }
        let home = std::env::var_os("HOME").context("HOME is unavailable")?;
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
    candidates.into_iter().find(|path| path.is_dir()).unwrap_or_else(|| home.join("Pictures/Nintendo Switch"))
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
