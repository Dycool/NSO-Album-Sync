//! Nintendo Switch Online Coral client, credential cache, and service-token broker.

use crate::http::{DEFAULT_GET_RESPONSE_LIMIT, HttpClient, HttpResponse};
use crate::model::{MediaItem, NintendoPresence};
use crate::nintendo_auth::NintendoAuthManager;
use crate::nxapi::{FAttestation, NxapiClient};
use crate::secure_store::SecureStore;
use crate::util::{json_i64, json_string, sha256_base64url};
use serde_json::{Value, json};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const CORAL_BASE: &str = "https://api-lp1.znc.srv.nintendo.net";
const LOGIN_PATH: &str = "/v4/Account/Login";
const MEDIA_LIST_PATH: &str = "/v4/Media/List";
const SHOW_SELF_PATH: &str = "/v4/User/ShowSelf";
const WEB_SERVICE_TOKEN_PATH: &str = "/v4/Game/GetWebServiceToken";
const CORAL_CREDENTIAL_ACCOUNT: &str = "CoralCredential";
const WEB_SERVICE_CREDENTIALS_ACCOUNT: &str = "GameWebServiceTokens";
const LEGACY_WEB_SERVICE_CREDENTIAL_ACCOUNT: &str = "GameWebServiceToken";
const WORKER_BASE: &str = "https://nso-worker-backend.diogoenes0.workers.dev";
const WORKER_CLIENT_ID: &str = "nso-album-sync";
const DEFAULT_GAME_SERVICE_ID: u64 = 4_834_290_508_791_808;
const METHOD1_LIMIT: usize = 10;
const METHOD1_WINDOW: u64 = 60 * 60;
const METHOD2_LIMIT: usize = 20;
const METHOD2_WINDOW: u64 = 30 * 60;
const WORKER_BREAKER_SECONDS: u64 = 6 * 60 * 60;

#[derive(Debug, Clone)]
struct CachedWebServiceToken {
    token: String,
    expires_at: u64,
}

#[derive(Default)]
struct CoralState {
    session_hash: String,
    access_token: String,
    user_id: String,
    na_id: String,
    expires_at: u64,
    generation: u64,
    web_tokens: HashMap<u64, CachedWebServiceToken>,
    web_tokens_loaded_for_hash: String,
}

#[derive(Default)]
struct WorkerHealth {
    method1_disabled_until: u64,
    method2_disabled_until: u64,
}

#[derive(Default)]
struct FallbackLimits {
    method1: HashMap<String, VecDeque<u64>>,
    method2: HashMap<String, VecDeque<u64>>,
}

pub struct CoralClient {
    http: HttpClient,
    auth: Arc<NintendoAuthManager>,
    nxapi: Arc<NxapiClient>,
    state: Mutex<CoralState>,
    login_lock: Mutex<()>,
    request_lock: Mutex<()>,
    worker_health: Mutex<WorkerHealth>,
    fallback_limits: Mutex<FallbackLimits>,
}

impl CoralClient {
    pub fn new(http: HttpClient, auth: Arc<NintendoAuthManager>, nxapi: Arc<NxapiClient>) -> Self {
        Self {
            http,
            auth,
            nxapi,
            state: Mutex::new(CoralState::default()),
            login_lock: Mutex::new(()),
            request_lock: Mutex::new(()),
            worker_health: Mutex::new(WorkerHealth::default()),
            fallback_limits: Mutex::new(FallbackLimits::default()),
        }
    }

    pub fn clear_cached_session(&self) {
        {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.generation = state.generation.wrapping_add(1);
            state.session_hash.clear();
            state.access_token.clear();
            state.user_id.clear();
            state.na_id.clear();
            state.expires_at = 0;
            state.web_tokens.clear();
            state.web_tokens_loaded_for_hash.clear();
        }
        self.nxapi.clear_user_auth();
        let _ = SecureStore::erase(CORAL_CREDENTIAL_ACCOUNT);
        let _ = SecureStore::erase(WEB_SERVICE_CREDENTIALS_ACCOUNT);
        let _ = SecureStore::erase(LEGACY_WEB_SERVICE_CREDENTIAL_ACCOUNT);
    }

    pub fn media_list(&self, session_token: &str) -> anyhow::Result<Vec<MediaItem>> {
        let access_token = self.ensure_session(session_token)?;
        let response = self.coral_call(MEDIA_LIST_PATH, &access_token, &json!({"parameter": {}}))?;
        let Some(media) = response.get("result").and_then(|result| result.get("media")).and_then(Value::as_array) else {
            return Ok(Vec::new());
        };
        let mut output = Vec::with_capacity(media.len());
        for item in media {
            output.push(serde_json::from_value::<MediaItem>(item.clone())?);
        }
        Ok(output)
    }

    pub fn self_presence(&self, session_token: &str) -> anyhow::Result<NintendoPresence> {
        let access_token = self.ensure_session(session_token)?;
        let user_id = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).user_id.clone();
        let parameter = if user_id.is_empty() {
            json!({"parameter": {}})
        } else if let Ok(number) = user_id.parse::<u64>() {
            json!({"parameter": {"id": number}})
        } else {
            json!({"parameter": {"id": user_id}})
        };
        let response = self.coral_call(SHOW_SELF_PATH, &access_token, &parameter)?;
        Ok(response.get("result").map(NintendoPresence::from_coral_result).unwrap_or_default())
    }

    pub fn get_web_service_token(&self, session_token: &str, game_service_id: u64) -> anyhow::Result<String> {
        let target_id = if game_service_id == 0 { DEFAULT_GAME_SERVICE_ID } else { game_service_id };
        let session_hash = sha256_base64url(session_token);
        let now = now_seconds();
        self.restore_web_service_tokens(&session_hash);
        {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.session_hash == session_hash
                && let Some(cached) = state.web_tokens.get(&target_id)
                && !cached.token.is_empty()
                && now < cached.expires_at
            {
                return Ok(cached.token.clone());
            }
        }

        let access_token = self.ensure_session(session_token)?;
        let (mut na_id, coral_user_id, generation) = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            (state.na_id.clone(), state.user_id.clone(), state.generation)
        };
        if na_id.is_empty() {
            let tokens = self.auth.exchange_session_token(session_token)?;
            let profile = self.auth.fetch_profile(tokens.access_token())?;
            na_id = profile.id().to_owned();
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.generation != generation { anyhow::bail!("Coral authentication cancelled"); }
            state.na_id = na_id.clone();
        }

        let _request_guard = self.request_lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let (attestation, used_worker) = self.preferred_attestation(2, &access_token, &na_id, &coral_user_id)?;
        let mut attempt = self.perform_web_service_token_request(target_id, &access_token, &attestation)?;
        let mut recovered_from_worker = false;
        if used_worker && is_f_rejection_payload(&attempt.1) {
            let fallback = self.limited_nxapi_attestation(2, &access_token, &na_id, &coral_user_id)?;
            attempt = self.perform_web_service_token_request(target_id, &access_token, &fallback)?;
            recovered_from_worker = true;
        }
        let (response, decoded) = attempt;
        if response.status() == 401 || response.status() == 403 { self.clear_cached_session(); }
        anyhow::ensure!(response.status() / 100 == 2, "Coral GetWebServiceToken failed (HTTP {}): {}", response.status(), decoded);
        anyhow::ensure!(!decoded.is_empty(), "Coral GetWebServiceToken returned an empty response");
        let root: Value = serde_json::from_str(&decoded)?;
        let Some(result) = root.get("result") else { anyhow::bail!("Coral GetWebServiceToken response missing result"); };
        let token = json_string(result, "accessToken");
        anyhow::ensure!(!token.is_empty(), "Coral GetWebServiceToken response missing accessToken");
        let expires_in = json_i64(result, "expiresIn", 10_800).max(180) as u64;
        let expires_at = now_seconds().saturating_add(expires_in.saturating_sub(120).max(60));
        if recovered_from_worker { self.disable_worker(2); }
        {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            anyhow::ensure!(state.generation == generation && state.session_hash == session_hash, "Coral authentication cancelled");
            state.web_tokens.insert(target_id, CachedWebServiceToken { token: token.clone(), expires_at });
        }
        self.persist_web_service_tokens(&session_hash);
        Ok(token)
    }

    fn ensure_session(&self, session_token: &str) -> anyhow::Result<String> {
        let _login_guard = self.login_lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let wanted_hash = sha256_base64url(session_token);
        let now = now_seconds();
        {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.session_hash == wanted_hash && !state.access_token.is_empty() && now < state.expires_at {
                return Ok(state.access_token.clone());
            }
        }
        if let Some(restored) = self.restore_persistent_session(&wanted_hash, now) { return Ok(restored); }

        let generation = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).generation;
        let nintendo_tokens = self.auth.exchange_session_token(session_token)?;
        let profile = self.auth.fetch_profile(nintendo_tokens.access_token())?;
        let (attestation, used_worker) = self.preferred_attestation(1, nintendo_tokens.id_token(), profile.id(), "")?;
        let mut attempt = self.perform_login_request(nintendo_tokens.id_token(), &profile, &attestation)?;
        let mut recovered_from_worker = false;
        if used_worker && is_f_rejection_payload(&attempt.1) {
            let fallback = self.limited_nxapi_attestation(1, nintendo_tokens.id_token(), profile.id(), "")?;
            attempt = self.perform_login_request(nintendo_tokens.id_token(), &profile, &fallback)?;
            recovered_from_worker = true;
        }
        let (response, decoded) = attempt;
        anyhow::ensure!(response.status() / 100 == 2, "Coral login failed (HTTP {}): {}", response.status(), decoded);
        anyhow::ensure!(!decoded.is_empty(), "Coral login returned an empty response");
        let root: Value = serde_json::from_str(&decoded)?;
        let result = root.get("result").ok_or_else(|| anyhow::anyhow!("Coral login missing result"))?;
        let credential = result.get("webApiServerCredential").ok_or_else(|| anyhow::anyhow!("Coral login missing credential"))?;
        let access_token = json_string(credential, "accessToken");
        anyhow::ensure!(!access_token.is_empty(), "Coral login missing access token");
        let expires_in = json_i64(credential, "expiresIn", 7_200).max(1) as u64;
        let user_id = result.get("user").and_then(|user| user.get("id")).and_then(value_as_id).unwrap_or_default();
        if recovered_from_worker { self.disable_worker(1); }
        {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            anyhow::ensure!(state.generation == generation, "Coral authentication cancelled");
            state.session_hash = wanted_hash.clone();
            state.access_token = access_token.clone();
            state.user_id = user_id;
            state.na_id = profile.id().to_owned();
            state.expires_at = now_seconds().saturating_add(expires_in);
            state.web_tokens.retain(|_, cached| now_seconds() < cached.expires_at);
        }
        self.persist_session(&wanted_hash);
        Ok(access_token)
    }

    fn coral_call(&self, path: &str, access_token: &str, body: &Value) -> anyhow::Result<Value> {
        let _request_guard = self.request_lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let version = self.nxapi.nso_version()?;
        let url = format!("{CORAL_BASE}{path}");
        let plaintext = serde_json::to_string(body)?;
        let encrypted = self.nxapi.encrypt_request(&url, access_token, &plaintext)?;
        let response = self.http.post_bytes(&url, &encrypted, &[
            format!("Authorization: Bearer {access_token}"),
            format!("User-Agent: com.nintendo.znca/{version}(Android/12)"),
            "Content-Type: application/octet-stream".to_owned(),
            "Accept: application/octet-stream,application/json".to_owned(),
        ], 30, DEFAULT_GET_RESPONSE_LIMIT)?;
        if response.status() == 401 || response.status() == 403 { self.clear_cached_session(); }
        let decoded = decode_coral_response(&self.nxapi, &response)?;
        anyhow::ensure!(response.status() / 100 == 2, "Coral request failed (HTTP {}): {}", response.status(), decoded);
        anyhow::ensure!(!decoded.is_empty(), "Coral request returned an empty response");
        Ok(serde_json::from_str(&decoded)?)
    }

    fn perform_login_request(&self, id_token: &str, profile: &crate::nintendo_auth::UserProfile, f: &FAttestation) -> anyhow::Result<(HttpResponse, String)> {
        let version = self.nxapi.nso_version()?;
        let url = format!("{CORAL_BASE}{LOGIN_PATH}");
        let body = json!({"parameter": {
            "naIdToken": id_token,
            "naBirthday": profile.birthday(),
            "naCountry": profile.country(),
            "language": profile.language(),
            "f": f.f(),
            "requestId": f.request_id(),
            "timestamp": f.timestamp()
        }});
        let encrypted = self.nxapi.encrypt_request(&url, "", &serde_json::to_string(&body)?)?;
        let response = self.http.post_bytes(&url, &encrypted, &[
            "X-Platform: Android".to_owned(),
            format!("X-ProductVersion: {version}"),
            format!("User-Agent: com.nintendo.znca/{version}(Android/12)"),
            "Content-Type: application/octet-stream".to_owned(),
            "Accept: application/octet-stream,application/json".to_owned(),
        ], 30, DEFAULT_GET_RESPONSE_LIMIT)?;
        let decoded = decode_coral_response(&self.nxapi, &response)?;
        Ok((response, decoded))
    }

    fn perform_web_service_token_request(&self, service_id: u64, access_token: &str, f: &FAttestation) -> anyhow::Result<(HttpResponse, String)> {
        let version = self.nxapi.nso_version()?;
        let url = format!("{CORAL_BASE}{WEB_SERVICE_TOKEN_PATH}");
        let body = json!({"parameter": {
            "id": service_id,
            "registrationToken": "",
            "f": f.f(),
            "requestId": f.request_id(),
            "timestamp": f.timestamp()
        }});
        let encrypted = self.nxapi.encrypt_request(&url, access_token, &serde_json::to_string(&body)?)?;
        let response = self.http.post_bytes(&url, &encrypted, &[
            "Content-Type: application/octet-stream".to_owned(),
            "Accept: application/octet-stream,application/json".to_owned(),
            "Accept-Language: en-US".to_owned(),
            format!("Authorization: Bearer {access_token}"),
            "X-Platform: Android".to_owned(),
            format!("X-ProductVersion: {version}"),
            format!("User-Agent: com.nintendo.znca/{version}(Android/12)"),
        ], 30, DEFAULT_GET_RESPONSE_LIMIT)?;
        let decoded = decode_coral_response(&self.nxapi, &response)?;
        Ok((response, decoded))
    }

    fn preferred_attestation(&self, method: i32, token: &str, na_id: &str, coral_user_id: &str) -> anyhow::Result<(FAttestation, bool)> {
        if self.worker_enabled(method)
            && let Ok(attestation) = self.worker_attestation(method, token, na_id, coral_user_id)
        {
            return Ok((attestation, true));
        }
        Ok((self.limited_nxapi_attestation(method, token, na_id, coral_user_id)?, false))
    }

    fn worker_attestation(&self, method: i32, token: &str, na_id: &str, coral_user_id: &str) -> anyhow::Result<FAttestation> {
        let response = self.http.post_json(&format!("{WORKER_BASE}/api/nso/f"), &json!({
            "clientId": WORKER_CLIENT_ID,
            "hashMethod": method.to_string(),
            "token": token,
            "naId": na_id,
            "coralUserId": coral_user_id
        }), &["Accept: application/json".to_owned(), "User-Agent: nso-album-sync/2.0.0".to_owned()], 30)?;
        anyhow::ensure!(response.status() / 100 == 2, "native f-token generation failed (HTTP {})", response.status());
        let json: Value = serde_json::from_slice(response.body())?;
        let result = FAttestation::from_parts(
            json_string(&json, "f"),
            json.get("requestId").and_then(Value::as_str).or_else(|| json.get("request_id").and_then(Value::as_str)).unwrap_or_default().to_owned(),
            json_i64(&json, "timestamp", 0),
        );
        anyhow::ensure!(!result.f().is_empty() && !result.request_id().is_empty() && result.timestamp() != 0, "native f-token response was incomplete");
        Ok(result)
    }

    fn limited_nxapi_attestation(&self, method: i32, token: &str, na_id: &str, coral_user_id: &str) -> anyhow::Result<FAttestation> {
        let now = now_seconds();
        let fallback_key = sha256_base64url(token);
        let mut limits = self.fallback_limits.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let (attempts, window, maximum, label) = match method {
            1 => (limits.method1.entry(if na_id.is_empty() { fallback_key } else { na_id.to_owned() }).or_default(), METHOD1_WINDOW, METHOD1_LIMIT, "method-1"),
            2 => (limits.method2.entry(if coral_user_id.is_empty() { fallback_key } else { coral_user_id.to_owned() }).or_default(), METHOD2_WINDOW, METHOD2_LIMIT, "method-2"),
            _ => anyhow::bail!("unsupported nxapi f hash method"),
        };
        while attempts.front().is_some_and(|oldest| now.saturating_sub(*oldest) >= window) { attempts.pop_front(); }
        anyhow::ensure!(attempts.len() < maximum, "nxapi {label} f-token local limit reached");
        attempts.push_back(now);
        drop(limits);
        self.nxapi.generate_f(method, token, na_id, coral_user_id)
    }

    fn worker_enabled(&self, method: i32) -> bool {
        let health = self.worker_health.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let now = now_seconds();
        match method { 1 => now >= health.method1_disabled_until, 2 => now >= health.method2_disabled_until, _ => false }
    }

    fn disable_worker(&self, method: i32) {
        let until = now_seconds().saturating_add(WORKER_BREAKER_SECONDS);
        let mut health = self.worker_health.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if method == 1 { health.method1_disabled_until = until; }
        if method == 2 { health.method2_disabled_until = until; }
    }

    fn restore_persistent_session(&self, wanted_hash: &str, now: u64) -> Option<String> {
        let stored = SecureStore::get(CORAL_CREDENTIAL_ACCOUNT).ok().flatten()?;
        let json: Value = serde_json::from_str(&stored).ok()?;
        if json_string(&json, "sessionHash") != wanted_hash { return None; }
        let expires_at = json.get("expiresAt").and_then(Value::as_u64).unwrap_or(0);
        let token = json_string(&json, "accessToken");
        if token.is_empty() || now >= expires_at {
            let _ = SecureStore::erase(CORAL_CREDENTIAL_ACCOUNT);
            return None;
        }
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.session_hash = wanted_hash.to_owned();
        state.access_token = token.clone();
        state.user_id = json_string(&json, "userId");
        state.expires_at = expires_at;
        Some(token)
    }

    fn persist_session(&self, session_hash: &str) {
        if !SecureStore::available() { return; }
        let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let payload = json!({"sessionHash": session_hash, "accessToken": state.access_token, "userId": state.user_id, "expiresAt": state.expires_at});
        let _ = SecureStore::put(CORAL_CREDENTIAL_ACCOUNT, &payload.to_string());
    }

    fn restore_web_service_tokens(&self, session_hash: &str) {
        let already_loaded = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.web_tokens_loaded_for_hash == session_hash
        };
        if already_loaded { return; }
        let _ = SecureStore::erase(LEGACY_WEB_SERVICE_CREDENTIAL_ACCOUNT);
        let stored = SecureStore::get(WEB_SERVICE_CREDENTIALS_ACCOUNT).ok().flatten();
        let now = now_seconds();
        let mut restored = HashMap::new();
        if let Some(stored) = stored
            && let Ok(json) = serde_json::from_str::<Value>(&stored)
        {
            if json_string(&json, "sessionHash") == session_hash {
                if let Some(tokens) = json.get("tokens").and_then(Value::as_object) {
                    for (id, record) in tokens {
                        let Ok(service_id) = id.parse::<u64>() else { continue; };
                        let token = json_string(record, "accessToken");
                        let expires_at = record.get("expiresAt").and_then(Value::as_u64).unwrap_or(0);
                        if !token.is_empty() && now < expires_at {
                            restored.insert(service_id, CachedWebServiceToken { token, expires_at });
                        }
                    }
                }
            } else {
                let _ = SecureStore::erase(WEB_SERVICE_CREDENTIALS_ACCOUNT);
            }
        }
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.web_tokens.extend(restored);
        state.web_tokens_loaded_for_hash = session_hash.to_owned();
    }

    fn persist_web_service_tokens(&self, session_hash: &str) {
        if !SecureStore::available() { return; }
        let now = now_seconds();
        let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let mut tokens = serde_json::Map::new();
        for (service_id, cached) in &state.web_tokens {
            if now < cached.expires_at {
                tokens.insert(service_id.to_string(), json!({"accessToken": cached.token, "expiresAt": cached.expires_at}));
            }
        }
        let payload = json!({"sessionHash": session_hash, "tokens": tokens});
        let _ = SecureStore::put(WEB_SERVICE_CREDENTIALS_ACCOUNT, &payload.to_string());
        let _ = SecureStore::erase(LEGACY_WEB_SERVICE_CREDENTIAL_ACCOUNT);
    }
}

fn decode_coral_response(nxapi: &NxapiClient, response: &HttpResponse) -> anyhow::Result<String> {
    if response.body().is_empty() { return Ok(String::new()); }
    match nxapi.decrypt_response(response.body()) {
        Ok(decoded) => Ok(decoded),
        Err(error) => {
            let raw = response.text();
            let first = raw.trim_start().chars().next();
            if matches!(first, Some('{') | Some('[')) { Ok(raw) } else if response.status() / 100 == 2 { Err(error) } else { Ok(String::new()) }
        }
    }
}

fn is_f_rejection_payload(payload: &str) -> bool {
    let Ok(json) = serde_json::from_str::<Value>(payload) else { return false; };
    let status = json_i64(&json, "status", 0);
    let message = json_string(&json, "errorMessage");
    matches!(status, 9403 | 9599) || matches!(message.as_str(), "Invalid token." | "Unexpected error.")
}

fn value_as_id(value: &Value) -> Option<String> {
    value.as_str().map(ToOwned::to_owned)
        .or_else(|| value.as_u64().map(|number| number.to_string()))
        .or_else(|| value.as_i64().map(|number| number.to_string()))
}

fn now_seconds() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or(Duration::ZERO).as_secs()
}
