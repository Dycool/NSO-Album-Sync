//! nxapi-znca-api integration used for Nintendo attestation and Coral request crypto.

use crate::http::{DEFAULT_GET_RESPONSE_LIMIT, HttpClient, HttpResponse};
use crate::nintendo_auth::UserProfile;
use crate::util::{base64_decode, base64_standard, json_i64, json_string};
use atomic_write_file::AtomicWriteFile;
use serde_json::json;
use std::fs;
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const ZNCA_BASE: &str = "https://nxapi-znca-api.fancy.org.uk/api/znca";
const AUTH_URL: &str = "https://nxapi-auth.fancy.org.uk/api/oauth/token";
const USER_AGENT: &str = "nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)";
const CLIENT_VERSION: &str = "w8zSLBsxR7rVoGJA";

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

    pub fn f(&self) -> &str {
        &self.f
    }

    pub fn request_id(&self) -> &str {
        &self.request_id
    }

    pub fn timestamp(&self) -> i64 {
        self.timestamp
    }
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
        state.auth_token.clear();
        state.refresh_token.clear();
        state.auth_expiry = 0;
        state.generation = state.generation.wrapping_add(1);
    }

    pub fn nso_version(&self) -> anyhow::Result<String> {
        let now = now_seconds();
        {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if !state.version.is_empty() && now < state.version_expiry {
                return Ok(state.version.clone());
            }
        }

        match self.znca_request("GET", "/config", "", "application/json", &[]) {
            Ok(response) if response.status() / 100 == 2 => {
                let body: serde_json::Value = serde_json::from_slice(response.body())?;
                let version = json_string(&body, "nso_version");
                anyhow::ensure!(!version.is_empty(), "nxapi config response missing nso_version");
                {
                    let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                    state.version = version.clone();
                    state.version_expiry = now.saturating_add(6 * 60 * 60);
                }
                self.save_cache();
                Ok(version)
            }
            Ok(response) => self.stale_version_or_error(format!(
                "nxapi config failed (HTTP {})",
                response.status()
            )),
            Err(error) => self.stale_version_or_error(error.to_string()),
        }
    }

    fn stale_version_or_error(&self, message: String) -> anyhow::Result<String> {
        let value = {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.version.is_empty() {
                None
            } else {
                state.version_expiry = now_seconds().saturating_add(15 * 60);
                Some(state.version.clone())
            }
        };
        if let Some(value) = value {
            self.save_cache();
            Ok(value)
        } else {
            anyhow::bail!(message)
        }
    }

    pub fn generate_f(
        &self,
        hash_method: i32,
        token: &str,
        na_id: &str,
        coral_user_id: &str,
    ) -> anyhow::Result<FAttestation> {
        let _ = self.nso_version()?;
        let body = json!({
            "hash_method": hash_method.to_string(),
            "token": token,
            "na_id": na_id,
            "coral_user_id": coral_user_id,
        });
        let response = self.znca_request(
            "POST",
            "/f",
            &serde_json::to_string(&body)?,
            "application/json",
            &[],
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi f request failed (HTTP {})",
            response.status()
        );
        let body: serde_json::Value = serde_json::from_slice(response.body())?;
        let result = body.get("result").unwrap_or(&body);
        let output = FAttestation::from_parts(
            json_string(result, "f"),
            json_string(result, "request_id"),
            json_i64(result, "timestamp", 0),
        );
        anyhow::ensure!(
            !output.f.is_empty() && !output.request_id.is_empty() && output.timestamp > 0,
            "nxapi returned incomplete f attestation"
        );
        Ok(output)
    }

    pub fn encrypted_login_body(
        &self,
        id_token: &str,
        profile: &UserProfile,
    ) -> anyhow::Result<Vec<u8>> {
        let attestation = self.generate_f(1, id_token, profile.id(), "")?;
        let body = json!({"parameter": {
            "naIdToken": id_token,
            "birthday": profile.birthday(),
            "country": profile.country(),
            "language": profile.language(),
            "f": attestation.f(),
            "requestId": attestation.request_id(),
            "timestamp": attestation.timestamp()
        }});
        self.encrypt_request(
            "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login",
            "",
            &serde_json::to_string(&body)?,
        )
    }

    pub fn encrypted_web_service_token_body(
        &self,
        coral_access_token: &str,
        na_id: &str,
        coral_user_id: &str,
        game_service_id: u64,
    ) -> anyhow::Result<Vec<u8>> {
        let attestation = self.generate_f(2, coral_access_token, na_id, coral_user_id)?;
        let body = json!({"parameter": {
            "id": game_service_id,
            "f": attestation.f(),
            "requestId": attestation.request_id(),
            "timestamp": attestation.timestamp()
        }});
        self.encrypt_request(
            "https://api-lp1.znc.srv.nintendo.net/v4/Game/GetWebServiceToken",
            coral_access_token,
            &serde_json::to_string(&body)?,
        )
    }

    pub fn encrypt_request(
        &self,
        url: &str,
        coral_token: &str,
        plaintext_json: &str,
    ) -> anyhow::Result<Vec<u8>> {
        let _ = self.nso_version()?;
        let body = json!({
            "url": url,
            "token": if coral_token.is_empty() {
                serde_json::Value::Null
            } else {
                serde_json::Value::String(coral_token.to_owned())
            },
            "data": serde_json::from_str::<serde_json::Value>(plaintext_json)
                .unwrap_or_else(|_| serde_json::Value::String(plaintext_json.to_owned()))
        });
        let response = self.znca_request(
            "POST",
            "/encrypt-request",
            &serde_json::to_string(&body)?,
            "application/json",
            &[],
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi encrypt request failed (HTTP {})",
            response.status()
        );
        let body: serde_json::Value = serde_json::from_slice(response.body())?;
        let encoded = json_string(&body, "data");
        anyhow::ensure!(!encoded.is_empty(), "nxapi encrypt response missing data");
        base64_decode(&encoded)
    }

    pub fn decrypt_response(&self, encrypted: &[u8]) -> anyhow::Result<String> {
        let _ = self.nso_version()?;
        let body = json!({"data": base64_standard(encrypted)});
        let response = self.znca_request(
            "POST",
            "/decrypt-response",
            &serde_json::to_string(&body)?,
            "text/plain,application/json",
            &[],
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi decrypt response failed (HTTP {})",
            response.status()
        );
        let text = response.text();
        if let Ok(body) = serde_json::from_str::<serde_json::Value>(&text) {
            let data = json_string(&body, "data");
            if !data.is_empty() {
                return Ok(data);
            }
        }
        Ok(text)
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
        self.throw_if_rate_limited()?;
        let token = self.auth_token()?;
        let version = self
            .state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .version
            .clone();
        let mut headers = vec![
            format!("Authorization: Bearer {token}"),
            format!("User-Agent: {USER_AGENT}"),
            format!("Accept: {accept}"),
            "X-znca-Platform: Android".to_owned(),
            format!("X-znca-Client-Version: {CLIENT_VERSION}"),
        ];
        if !version.is_empty() {
            headers.push(format!("X-znca-Version: {version}"));
        }
        headers.extend_from_slice(extra_headers);
        let url = format!("{ZNCA_BASE}{path}");
        let response = if method == "GET" {
            self.http
                .get(&url, &headers, 30, DEFAULT_GET_RESPONSE_LIMIT)?
        } else {
            self.http.post_text(
                &url,
                body,
                &headers,
                "application/json",
                30,
                DEFAULT_GET_RESPONSE_LIMIT,
            )?
        };
        self.apply_rate_limit_response(&response);
        if response.status() == 401 {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.auth_token.clear();
            state.auth_expiry = 0;
        }
        if response.status() == 406 {
            {
                let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                state.version.clear();
                state.version_expiry = 0;
            }
            self.save_cache();
        }
        Ok(response)
    }

    fn auth_token(&self) -> anyhow::Result<String> {
        let now = now_seconds();
        let (refresh, generation) = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if !state.auth_token.is_empty() && now < state.auth_expiry {
                return Ok(state.auth_token.clone());
            }
            (state.refresh_token.clone(), state.generation)
        };
        anyhow::ensure!(
            !self.client_id.trim().is_empty(),
            "nxapi auth client id is missing"
        );

        let mut response = None;
        let mut previous_refresh = refresh.clone();
        if !refresh.is_empty() {
            let fields = [
                ("grant_type", "refresh_token"),
                ("client_id", self.client_id.as_str()),
                ("scope", "ca:gf ca:er ca:dr"),
                ("refresh_token", refresh.as_str()),
            ];
            let refresh_response = self.http.post_form(
                AUTH_URL,
                &fields,
                &[format!("User-Agent: {USER_AGENT}"), "Accept: application/json".to_owned()],
                30,
            )?;
            self.apply_rate_limit_response(&refresh_response);
            if refresh_response.status() / 100 == 2 {
                response = Some(refresh_response);
            } else {
                let error_code = serde_json::from_slice::<serde_json::Value>(refresh_response.body())
                    .ok()
                    .map(|value| json_string(&value, "error"))
                    .unwrap_or_default();
                anyhow::ensure!(
                    error_code == "invalid_grant",
                    "nxapi auth refresh failed (HTTP {}): {}",
                    refresh_response.status(),
                    refresh_response.text()
                );
                previous_refresh.clear();
                let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                if state.generation == generation {
                    state.refresh_token.clear();
                }
            }
        }

        let response = match response {
            Some(response) => response,
            None => {
                let fields = [
                    ("grant_type", "client_credentials"),
                    ("client_id", self.client_id.as_str()),
                    ("scope", "ca:gf ca:er ca:dr"),
                ];
                let response = self.http.post_form(
                    AUTH_URL,
                    &fields,
                    &[format!("User-Agent: {USER_AGENT}"), "Accept: application/json".to_owned()],
                    30,
                )?;
                self.apply_rate_limit_response(&response);
                response
            }
        };
        anyhow::ensure!(
            response.status() / 100 == 2,
            "nxapi auth failed (HTTP {}): {}",
            response.status(),
            response.text()
        );
        let body: serde_json::Value = serde_json::from_slice(response.body())?;
        let access = json_string(&body, "access_token");
        anyhow::ensure!(!access.is_empty(), "nxapi auth response missing access_token");
        let expires = json_i64(&body, "expires_in", 3600).max(1) as u64;
        let next_refresh = body
            .get("refresh_token")
            .and_then(serde_json::Value::as_str)
            .unwrap_or(&previous_refresh)
            .to_owned();

        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        anyhow::ensure!(
            state.generation == generation,
            "nxapi user auth was cleared during token refresh"
        );
        state.auth_token = access.clone();
        state.refresh_token = next_refresh;
        state.auth_expiry = now_seconds().saturating_add(expires);
        Ok(access)
    }

    fn throw_if_rate_limited(&self) -> anyhow::Result<()> {
        let until = self
            .state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .rate_limit_until;
        anyhow::ensure!(
            now_seconds() >= until,
            "nxapi request is locally rate-limited until unix time {until}"
        );
        Ok(())
    }

    fn apply_rate_limit_response(&self, response: &HttpResponse) {
        if response.status() != 429 {
            return;
        }
        let retry = response
            .header("retry-after")
            .and_then(|value| value.trim().parse::<u64>().ok())
            .unwrap_or(15 * 60)
            .clamp(1, 60 * 60);
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .rate_limit_until = now_seconds().saturating_add(retry);
        self.save_cache();
    }

    fn load_cache(&self) {
        let Ok(bytes) = fs::read(&self.cache_file) else {
            return;
        };
        let Ok(body) = serde_json::from_slice::<serde_json::Value>(&bytes) else {
            return;
        };
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.version = json_string(&body, "nsoVersion");
        state.version_expiry = body
            .get("versionExpiry")
            .and_then(serde_json::Value::as_u64)
            .unwrap_or(0);
        state.rate_limit_until = body
            .get("rateLimitUntil")
            .and_then(serde_json::Value::as_u64)
            .unwrap_or(0);
    }

    fn save_cache(&self) {
        let snapshot = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            json!({
                "nsoVersion": state.version,
                "versionExpiry": state.version_expiry,
                "rateLimitUntil": state.rate_limit_until
            })
        };
        let Some(parent) = self.cache_file.parent() else {
            return;
        };
        if fs::create_dir_all(parent).is_err() {
            return;
        }
        let Ok(bytes) = serde_json::to_vec_pretty(&snapshot) else {
            return;
        };
        let Ok(mut file) = AtomicWriteFile::open(&self.cache_file) else {
            return;
        };
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

fn now_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or(Duration::ZERO)
        .as_secs()
}
