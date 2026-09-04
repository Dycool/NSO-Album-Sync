//! Pure utility helpers shared by the safe core and desktop integration.

use base64::{Engine as _, engine::general_purpose};
use sha2::{Digest, Sha256};

pub fn base64url(bytes: &[u8]) -> String {
    general_purpose::URL_SAFE_NO_PAD.encode(bytes)
}

pub fn base64_standard(bytes: &[u8]) -> String {
    general_purpose::STANDARD.encode(bytes)
}

pub fn base64_decode(input: &str) -> anyhow::Result<Vec<u8>> {
    general_purpose::URL_SAFE_NO_PAD.decode(input)
        .or_else(|_| general_purpose::STANDARD.decode(input))
        .map_err(Into::into)
}

pub fn sha256(input: impl AsRef<[u8]>) -> Vec<u8> {
    Sha256::digest(input.as_ref()).to_vec()
}

pub fn sha256_base64url(input: impl AsRef<[u8]>) -> String {
    base64url(&sha256(input))
}

pub fn trim(value: impl AsRef<str>) -> String {
    value.as_ref().trim().to_owned()
}

pub fn random_alphanumeric(length: usize) -> String {
    use rand::Rng as _;
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let mut rng = rand::rng();
    (0..length)
        .map(|_| {
            let index = rng.random_range(0..ALPHABET.len());
            char::from(ALPHABET[index])
        })
        .collect()
}

pub fn random_bytes(length: usize) -> Vec<u8> {
    use rand::RngCore as _;
    let mut output = vec![0_u8; length];
    rand::rng().fill_bytes(&mut output);
    output
}

pub fn unix_seconds_from_millis_or_seconds(value: i64) -> i64 {
    if value > 10_000_000_000 { value / 1000 } else { value }
}

pub fn json_string(value: &serde_json::Value, key: &str) -> String {
    value.get(key).and_then(|entry| entry.as_str()).unwrap_or_default().to_owned()
}

pub fn json_i64(value: &serde_json::Value, key: &str, fallback: i64) -> i64 {
    value.get(key)
        .and_then(|entry| entry.as_i64().or_else(|| entry.as_u64().and_then(|v| i64::try_from(v).ok())))
        .unwrap_or(fallback)
}

pub fn is_https_image_url(value: &str, max_len: usize) -> bool {
    !value.is_empty()
        && value.len() <= max_len
        && value.starts_with("https://")
        && !value.chars().any(char::is_whitespace)
}

#[cfg(test)]
mod tests {
    use super::{base64url, sha256, unix_seconds_from_millis_or_seconds};

    #[test]
    fn base64url_has_no_padding() {
        assert_eq!(base64url(&sha256("abc")), "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0");
    }

    #[test]
    fn timestamps_accept_millis_and_seconds() {
        assert_eq!(unix_seconds_from_millis_or_seconds(1_700_000_000), 1_700_000_000);
        assert_eq!(unix_seconds_from_millis_or_seconds(1_700_000_000_000), 1_700_000_000);
    }
}
