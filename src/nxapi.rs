//! nxapi-znca-api integration used for Nintendo attestation and Coral request crypto.

use crate::http::{DEFAULT_GET_RESPONSE_LIMIT, HttpClient, HttpResponse};
use crate::nintendo_auth::UserProfile;
use crate::util::{base64_decode, base64_standard, json_i64, json_string};
use atomic_write_file::AtomicWriteFile;
use chrono::{NaiveDateTime, Utc};
use serde_json::{Value, json};
use std::fs;
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const ZNCA_BASE: &str = "https://nxapi-znca-api.fancy.org.uk/api/znca";
const AUTH_URL: &str = "https://nxapi-auth.fancy.org.uk/api/oauth/token";
const USER_AGENT: &str = "nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)";
const CLIENT_VERSION: &str = "w8zSLBsxR7rVoGJA";
const CORAL_LOGIN_URL: &str = "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login";
const WEB_SERVICE_TOKEN_URL: &str = "https://api-lp1.znc.srv.nintendo.net/v4/Game/GetWebServiceToken";
const VERSION_CACHE_SECONDS: u64 = 6 * 60 * 60;
const VERSION_RETRY_SECONDS: u64 = 15 * 60;
const DEFAULT_RATE_LIMIT_BACKOFF_SECONDS: u64 = 15 * 60;

#[derive(Debug, Clone, Default)]
pub struct FAttestation {
    f: String,
    request_id: String,
    timestamp: i64,
}

impl FAttestation {
    pub(crate) fn from_parts(f: String, request_id: String, timestamp: i64) -> Self {
        Self { f, request_id, timestamp }
    }

    pub fn f(&self) -> &str { &self.f }
    pub fn request_id(&self) -> &str { &self.request_id }
    pub fn timestamp(&self) -> i64 { self.timestamp }
}

#[derive(Default)]
struct State {
    version: String,
    version_expiry: u64,
    auth_token: String,
    refresh_token: String,
    auth_expiry: u64,
    rate_limit_until: u64,
    generation: u64,
}

pub struct NxapiClient {
    http: HttpClient,
    client_id: String,
    cache_file: PathBuf,
    state: Mutex<State>,
    request_lock: Mutex<()>,
}

impl NxapiClient {
    pub fn new(http: HttpClient, client_id: impl Into<String>, cache_file: PathBuf) -> Self {
        let client = Self {
            http,
            client_id: client_id.into(),
            cache_file,
            state: Mutex::new(State::default()),
            request_lock: Mutex::new(()),
        };
        client.load_cache();
        client
    }

    pub fn clear_user_auth(&self) {
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.generation = state.generation.wrapping_add(1);
        state.auth_token.clear();
        state.refresh_token.clear();
        state.auth_expiry = 0;
    }

    pub fn nso_version(&self) -> anyhow::Result<String> {
        let _request_guard = self.request_lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            let now = now_seconds();
            if !state.version.is_empty() && now < state.version_expiry {
                return Ok(state.version.clone());
            }
        }

        let response = self.znca_request_unlocked("GET", "/config", "", "application/json", &[])?;
        let now = now_seconds();
        if response.status() / 100 == 2 {
            let body: Value = serde_json::from_slice(response.body())?;
            let version = json_string(&body, "nso_version");
            if !version.is_empty() {
                {
                    let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                    state.version = version.clone();
                    state.version_expiry = now.saturating_add(VERSION_CACHE_SECONDS);
                }
                self.save_cache();
                return Ok(version);
            }
        }

        let stale = {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.version.is_empty() {
                None
            } else {
                state.version_expiry = now.saturating_add(VERSION_RETRY_SECONDS);
                Some(state.version.clone())
            }
        };
        if let Some(version) = stale {
            self.save_cache();
            return Ok(version);
        }
        anyhow::bail!("Could not retrieve supported NSO version from nxapi /config")
    }

    pub fn encrypted_login_body(
        &self,
        id_token: &str,
        profile: &UserProfile,
    ) -> anyhow::Result<Vec<u8>> {
        let version = self.nso_version()?;
        let payload = json!({
            "token": id_token,
            "hash_method": "1",
            "na_id": profile.id(),
            "encrypt_token_request": {
                "url": CORAL_LOGIN_URL,
                "parameter": {
                    "naIdToken": id_token,
                    "naBirthday": profile.birthday(),
                    "naCountry": profile.country(),
                    "language": profile.language(),
                    "f": "",
                    "requestId": "",
                    "timestamp": 0
                }
            }
        });
        let response = self.znca_request(
            "POST",
            "/f",
            &serde_json::to_string(&payload)?,
            "application/json",
            &znca_headers(&version),
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi /f failed: {}",
            response.text()
        );
        let body: Value = serde_json::from_slice(response.body())?;
        let encrypted = json_string(&body, "encrypted_token_request");
        anyhow::ensure!(!encrypted.is_empty(), "nxapi /f missing encrypted_token_request");
        base64_decode(&encrypted)
    }

    pub fn generate_f(
        &self,
        hash_method: i32,
        token: &str,
        na_id: &str,
        coral_user_id: &str,
    ) -> anyhow::Result<FAttestation> {
        let version = self.nso_version()?;
        let payload = json!({
            "hash_method": hash_method.to_string(),
            "token": token,
            "na_id": na_id,
            "coral_user_id": coral_user_id,
        });
        let response = self.znca_request(
            "POST",
            "/f",
            &serde_json::to_string(&payload)?,
            "application/json",
            &znca_headers(&version),
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi /f failed: {}",
            response.text()
        );
        let body: Value = serde_json::from_slice(response.body())?;
        let output = FAttestation::from_parts(
            json_string(&body, "f"),
            json_string(&body, "request_id"),
            json_i64(&body, "timestamp", 0),
        );
        anyhow::ensure!(
            !output.f.is_empty() && !output.request_id.is_empty() && output.timestamp != 0,
            "nxapi /f returned incomplete attestation: {}",
            response.text()
        );
        Ok(output)
    }

    pub fn encrypted_web_service_token_body(
        &self,
        coral_access_token: &str,
        na_id: &str,
        coral_user_id: &str,
        game_service_id: u64,
    ) -> anyhow::Result<Vec<u8>> {
        let version = self.nso_version()?;
        let payload = json!({
            "token": coral_access_token,
            "hash_method": "2",
            "na_id": na_id,
            "coral_user_id": coral_user_id,
            "encrypt_token_request": {
                "url": WEB_SERVICE_TOKEN_URL,
                "parameter": {
                    "id": game_service_id,
                    "registrationToken": "",
                    "f": "",
                    "requestId": "",
                    "timestamp": 0
                }
            }
        });
        let response = self.znca_request(
            "POST",
            "/f",
            &serde_json::to_string(&payload)?,
            "application/json",
            &znca_headers(&version),
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi /f (hash_method 2) failed: {}",
            response.text()
        );
        let body: Value = serde_json::from_slice(response.body())?;
        let encrypted = json_string(&body, "encrypted_token_request");
        anyhow::ensure!(
            !encrypted.is_empty(),
            "nxapi /f (hash_method 2) missing encrypted_token_request"
        );
        base64_decode(&encrypted)
    }

    pub fn encrypt_request(
        &self,
        url: &str,
        coral_token: &str,
        plaintext_json: &str,
    ) -> anyhow::Result<Vec<u8>> {
        let version = self.nso_version()?;
        let payload = json!({
            "url": url,
            "token": if coral_token.is_empty() {
                Value::Null
            } else {
                Value::String(coral_token.to_owned())
            },
            "data": plaintext_json
        });
        let response = self.znca_request(
            "POST",
            "/encrypt-request",
            &serde_json::to_string(&payload)?,
            "application/json",
            &znca_headers(&version),
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi /encrypt-request failed: {}",
            response.text()
        );
        let body: Value = serde_json::from_slice(response.body())?;
        let encrypted = json_string(&body, "data");
        anyhow::ensure!(!encrypted.is_empty(), "nxapi /encrypt-request missing data");
        base64_decode(&encrypted)
    }

    pub fn decrypt_response(&self, encrypted: &[u8]) -> anyhow::Result<String> {
        let version = self.nso_version()?;
        let payload = json!({"data": base64_standard(encrypted)});
        let response = self.znca_request(
            "POST",
            "/decrypt-response",
            &serde_json::to_string(&payload)?,
            "text/plain",
            &znca_headers(&version),
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi /decrypt-response failed: {}",
            response.text()
        );
        Ok(response.text())
    }

    fn znca_request(
        &self,
        method: &str,
        path: &str,
        body: &str,
        accept: &str,
        extra_headers: &[String],
    ) -> anyhow::Result<HttpResponse> {
        let _request_guard = self.request_lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        self.znca_request_unlocked(method, path, body, accept, extra_headers)
    }

    fn znca_request_unlocked(
        &self,
        method: &str,
        path: &str,
        body: &str,
        accept: &str,
        extra_headers: &[String],
    ) -> anyhow::Result<HttpResponse> {
        self.throw_if_rate_limited()?;
        let bearer_token = self.auth_token()?;
        let mut headers = vec![
            format!("Accept: {accept}"),
            format!("User-Agent: {USER_AGENT}"),
        ];
        if !bearer_token.is_empty() {
            headers.push(format!("Authorization: Bearer {bearer_token}"));
        }
        headers.extend_from_slice(extra_headers);
        let url = format!("{ZNCA_BASE}{path}");
        let response = if method == "GET" {
            self.http.get(&url, &headers, 30, DEFAULT_GET_RESPONSE_LIMIT)?
        } else {
            self.http.post_text(
                &url,
                body,
                &headers,
                if body.is_empty() { "" } else { "application/json" },
                30,
                DEFAULT_GET_RESPONSE_LIMIT,
            )?
        };
        self.apply_rate_limit_response(&response);
        Ok(response)
    }

    fn auth_token(&self) -> anyhow::Result<String> {
        let (refresh_token, generation) = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            let now = now_seconds();
            if !state.auth_token.is_empty() && now < state.auth_expiry {
                return Ok(state.auth_token.clone());
            }
            (state.refresh_token.clone(), state.generation)
        };

        let request_token = |grant_type: &str, refresh: &str| -> anyhow::Result<HttpResponse> {
            let fields = if refresh.is_empty() {
                vec![
                    ("client_id", self.client_id.as_str()),
                    ("grant_type", grant_type),
                    ("scope", "ca:gf ca:er ca:dr"),
                ]
            } else {
                vec![
                    ("client_id", self.client_id.as_str()),
                    ("grant_type", grant_type),
                    ("refresh_token", refresh),
                    ("scope", "ca:gf ca:er ca:dr"),
                ]
            };
            let response = self.http.post_form(
                AUTH_URL,
                &fields,
                &[
                    "Accept: application/json".to_owned(),
                    format!("User-Agent: {USER_AGENT}"),
                ],
                30,
            )?;
            self.apply_rate_limit_response(&response);
            Ok(response)
        };

        let mut refresh = refresh_token;
        let mut response = if refresh.is_empty() {
            None
        } else {
            let response = request_token("refresh_token", &refresh)?;
            if response.status() / 100 == 2 {
                Some(response)
            } else {
                let error_code = serde_json::from_slice::<Value>(response.body())
                    .ok()
                    .map(|value| json_string(&value, "error"))
                    .unwrap_or_default();
                if error_code != "invalid_grant" {
                    anyhow::bail!("nxapi-auth refresh failed: {}", response.text());
                }
                refresh.clear();
                let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                if state.generation == generation {
                    state.refresh_token.clear();
                }
                None
            }
        };

        if response.is_none() {
            response = Some(request_token("client_credentials", "")?);
        }
        let response = response.expect("token response is always populated");
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi-auth token request failed: {}",
            response.text()
        );

        let body: Value = serde_json::from_slice(response.body())?;
        let access_token = json_string(&body, "access_token");
        let new_refresh_token = body
            .get("refresh_token")
            .and_then(Value::as_str)
            .unwrap_or(&refresh)
            .to_owned();
        anyhow::ensure!(!access_token.is_empty(), "nxapi-auth response missing access_token");
        let expires_in = json_i64(&body, "expires_in", 300).max(1) as u64;

        {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            anyhow::ensure!(state.generation == generation, "nxapi authentication cancelled");
            state.auth_token = access_token.clone();
            state.refresh_token = new_refresh_token;
            state.auth_expiry = now_seconds().saturating_add(expires_in);
        }
        Ok(access_token)
    }

    fn throw_if_rate_limited(&self) -> anyhow::Result<()> {
        let until = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).rate_limit_until;
        anyhow::ensure!(
            now_seconds() >= until,
            "nxapi-znca-api Retry-After backoff is active"
        );
        Ok(())
    }

    fn apply_rate_limit_response(&self, response: &HttpResponse) {
        if response.status() == 401 {
            self.clear_user_auth();
        }
        if response.status() == 406 {
            {
                let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                state.version.clear();
                state.version_expiry = 0;
            }
            self.save_cache();
        }
        if response.status() != 429 {
            return;
        }
        let delay = response
            .header("retry-after")
            .map(retry_after_delay)
            .unwrap_or(DEFAULT_RATE_LIMIT_BACKOFF_SECONDS);
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).rate_limit_until =
            now_seconds().saturating_add(delay);
        self.save_cache();
    }

    fn load_cache(&self) {
        let Ok(bytes) = fs::read(&self.cache_file) else { return; };
        let Ok(body) = serde_json::from_slice::<Value>(&bytes) else {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.version.clear();
            state.version_expiry = 0;
            state.rate_limit_until = 0;
            return;
        };
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.version = json_string(&body, "nsoVersion");
        state.version_expiry = body
            .get("nsoVersionExpiresAt")
            .and_then(Value::as_i64)
            .and_then(|value| u64::try_from(value).ok())
            .or_else(|| body.get("versionExpiry").and_then(Value::as_u64))
            .unwrap_or(0);
        state.rate_limit_until = body
            .get("rateLimitUntil")
            .and_then(Value::as_i64)
            .and_then(|value| u64::try_from(value).ok())
            .or_else(|| body.get("rateLimitUntil").and_then(Value::as_u64))
            .unwrap_or(0);
    }

    fn save_cache(&self) {
        let snapshot = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            json!({
                "nsoVersion": state.version,
                "nsoVersionExpiresAt": state.version_expiry,
                "rateLimitUntil": state.rate_limit_until
            })
        };
        let Some(parent) = self.cache_file.parent() else { return; };
        if fs::create_dir_all(parent).is_err() { return; }
        let Ok(bytes) = serde_json::to_vec(&snapshot) else { return; };
        let Ok(mut file) = AtomicWriteFile::open(&self.cache_file) else { return; };
        let commit_ok = file.write_all(&bytes).is_ok()
            && file.flush().is_ok()
            && file.commit().is_ok();
        #[cfg(unix)]
        if commit_ok {
            use std::os::unix::fs::PermissionsExt as _;
            let _ = fs::set_permissions(&self.cache_file, fs::Permissions::from_mode(0o600));
        }
        #[cfg(not(unix))]
        let _ = commit_ok;
    }
}

fn znca_headers(nso_version: &str) -> Vec<String> {
    vec![
        "X-znca-Platform: Android".to_owned(),
        format!("X-znca-Version: {nso_version}"),
        format!("X-znca-Client-Version: {CLIENT_VERSION}"),
    ]
}

fn retry_after_delay(value: &str) -> u64 {
    if let Ok(seconds) = value.trim().parse::<i64>() {
        return seconds.max(1) as u64;
    }
    if let Ok(retry_at) = NaiveDateTime::parse_from_str(value, "%a, %d %b %Y %H:%M:%S GMT") {
        let now = Utc::now().timestamp();
        return retry_at.and_utc().timestamp().saturating_sub(now).max(1) as u64;
    }
    DEFAULT_RATE_LIMIT_BACKOFF_SECONDS
}

fn now_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or(Duration::ZERO)
        .as_secs()
}
