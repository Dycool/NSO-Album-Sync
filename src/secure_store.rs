//! OS credential-store adapter. Secrets never fall back to plaintext files.

use keyring::v1::Entry;

const SERVICE: &str = "NSO Album Sync";

pub struct SecureStore;

impl SecureStore {
    pub fn available() -> bool {
        Entry::store_status().is_ok()
    }

    pub fn put(account: &str, secret: &str) -> anyhow::Result<()> {
        anyhow::ensure!(Self::available(), "OS credential store is unavailable");
        Entry::new(SERVICE, account)?.set_password(secret)?;
        Ok(())
    }

    pub fn get(account: &str) -> anyhow::Result<Option<String>> {
        if !Self::available() { return Ok(None); }
        let entry = Entry::new(SERVICE, account)?;
        match entry.get_password() {
            Ok(secret) => Ok(Some(secret)),
            Err(keyring::v1::Error::NoEntry) => Ok(None),
            Err(error) => Err(error.into()),
        }
    }

    pub fn erase(account: &str) -> anyhow::Result<()> {
        if !Self::available() { return Ok(()); }
        let entry = Entry::new(SERVICE, account)?;
        match entry.delete_credential() {
            Ok(()) | Err(keyring::v1::Error::NoEntry) => Ok(()),
            Err(error) => Err(error.into()),
        }
    }
}
