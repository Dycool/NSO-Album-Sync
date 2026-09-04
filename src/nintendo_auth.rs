//! Nintendo Account OAuth/PKCE flow and short-lived token cache.

use crate::auth_callback::{NINTENDO_REDIRECT_URI, is_nintendo_auth_callback};
use crate::http::HttpClient;
use crate::util::{base64url, json_i64, json_string, random_bytes, sha256};
use std::sync::Mutex;
use std::time::{Duration, Instant};

const CLIENT_ID: &str = "71b963c1b7b6d119";
const SCOPE: &str = "openid user user.birthday user.screenName";
const AUTHORIZE_URL: &str = "https://accounts.nintendo.com/connect/1.0.0/authorize";
const SESSION_TOKEN_URL: &str = "https://accounts.nintendo.com/connect/1.0.0/api/session_token";
const TOKEN_URL: &str = "https://accounts.nintendo.com/connect/1.0.0/api/token";
const PROFILE_URL: &str = "https://api.accounts.nintendo.com/2.0.0/users/me";

#[derive(Debug, Clone, Default)]
pub struct AuthResult {
    session_token: String,
    id_token: String,
    access_token: String,
    user_nickname: String,
}

impl AuthResult {
    pub fn session_token(&self) -> &str { &self.session_token }
    pub fn id_token(&self) -> &str { &self.id_token }
    pub fn access_token(&self) -> &str { &self.access_token }
    pub fn user_nickname(&self) -> &str { &self.user_nickname }
}

#[derive(Debug, Clone, Default)]
pub struct TokenResponse {
    id_token: String,
    access_token: String,
    expires_in: i64,
}

impl TokenResponse {
    pub fn id_token(&self) -> &str { &self.id_token }
    pub fn access_token(&self) -> &str { &self.access_token }
}

#[derive(Debug, Clone)]
pub struct UserProfile {
    id: String,
    nickname: String,
    birthday: String,
    country: String,
    language: String,
}

impl Default for UserProfile {
    fn default() -> Self {
        Self {
            id: String::new(),
            nickname: String::new(),
            birthday: "1995-01-01".to_owned(),
            country: "US".to_owned(),
            language: "en-GB".to_owned(),
        }
    }
}

impl UserProfile {
    pub fn id(&self) -> &str { &self.id }
    pub fn nickname(&self) -> &str { &self.nickname }
    pub fn birthday(&self) -> &str { &self.birthday }
    pub fn country(&self) -> &str { &self.country }
    pub fn language(&self) -> &str { &self.language }
}

#[derive(Default)]
struct Cache {
    oauth_state: String,
    pkce_verifier: String,
    session_token: String,
    tokens: TokenResponse,
    token_expiry: Option<Instant>,
    profile_access_token: String,
    profile: UserProfile,
    profile_valid: bool,
}

pub struct NintendoAuthManager {
    http: HttpClient,
    cache: Mutex<Cache>,
}

impl NintendoAuthManager {
    pub fn new(http: HttpClient) -> Self {
        Self { http, cache: Mutex::new(Cache::default()) }
    }

    pub fn authorize_url(&self) -> anyhow::Result<String> {
        let state = base64url(&random_bytes(36));
        let verifier = base64url(&random_bytes(32));
        let challenge = base64url(&sha256(verifier.as_bytes()));
        {
            let mut cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            cache.oauth_state = state.clone();
            cache.pkce_verifier = verifier;
        }
        let mut url = url::Url::parse(AUTHORIZE_URL)?;
        url.query_pairs_mut()
            .append_pair("state", &state)
            .append_pair("redirect_uri", NINTENDO_REDIRECT_URI)
            .append_pair("client_id", CLIENT_ID)
            .append_pair("scope", SCOPE)
            .append_pair("response_type", "session_token_code")
            .append_pair("session_token_code_challenge", &challenge)
            .append_pair("session_token_code_challenge_method", "S256")
            .append_pair("theme", "login_form");
        Ok(url.into())
    }

    pub fn complete_login(&self, callback_url: &str) -> anyhow::Result<AuthResult> {
        anyhow::ensure!(
            is_nintendo_auth_callback(callback_url),
            "Nintendo sign-in did not return through the registered browser callback"
        );
        let callback = url::Url::parse(callback_url)?;
        let returned_state = callback_parameter(&callback, "state");
        let error = callback_parameter(&callback, "error");
        let code = callback_parameter(&callback, "session_token_code");
        let (expected_state, verifier) = {
            let cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            anyhow::ensure!(
                !cache.oauth_state.is_empty() && !cache.pkce_verifier.is_empty(),
                "Nintendo sign-in session expired. Open the sign-in page again."
            );
            (cache.oauth_state.clone(), cache.pkce_verifier.clone())
        };
        anyhow::ensure!(
            !returned_state.is_empty() && returned_state == expected_state,
            "Nintendo sign-in callback had an invalid OAuth state"
        );
        if !error.is_empty() {
            anyhow::bail!(if error == "access_denied" {
                "Nintendo Account sign-in was cancelled.".to_owned()
            } else {
                format!("Nintendo Account sign-in failed: {error}")
            });
        }
        anyhow::ensure!(
            !code.is_empty(),
            "Nintendo sign-in callback did not include a session token code"
        );

        let session_token = self.exchange_code(&code, &verifier)?;
        {
            let mut cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            cache.oauth_state.clear();
            cache.pkce_verifier.clear();
        }
        let tokens = self.exchange_session_token(&session_token)?;
        let nickname = self
            .fetch_profile(tokens.access_token())
            .map(|profile| profile.nickname().to_owned())
            .unwrap_or_else(|_| "Nintendo Switch Player".to_owned());
        Ok(AuthResult {
            session_token,
            id_token: tokens.id_token.clone(),
            access_token: tokens.access_token.clone(),
            user_nickname: if nickname.is_empty() {
                "Nintendo Switch Player".to_owned()
            } else {
                nickname
            },
        })
    }

    fn exchange_code(&self, code: &str, verifier: &str) -> anyhow::Result<String> {
        let response = self.http.post_form(
            SESSION_TOKEN_URL,
            &[
                ("client_id", CLIENT_ID),
                ("session_token_code", code),
                ("session_token_code_verifier", verifier),
            ],
            &["User-Agent: NASDKAPI; Android".to_owned()],
            30,
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "Nintendo session-token exchange failed (HTTP {}): {}",
            response.status(),
            response.text()
        );
        let json: serde_json::Value = serde_json::from_slice(response.body())?;
        let token = json_string(&json, "session_token");
        anyhow::ensure!(!token.is_empty(), "Nintendo response missing session_token");
        Ok(token)
    }

    pub fn exchange_session_token(&self, session_token: &str) -> anyhow::Result<TokenResponse> {
        {
            let cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if cache.session_token == session_token
                && !cache.tokens.access_token.is_empty()
                && cache.token_expiry.is_some_and(|deadline| Instant::now() < deadline)
            {
                return Ok(cache.tokens.clone());
            }
        }
        let body = serde_json::json!({
            "client_id": CLIENT_ID,
            "session_token": session_token,
            "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer-session-token"
        });
        let response = self.http.post_json(
            TOKEN_URL,
            &body,
            &[
                "Accept: application/json".to_owned(),
                "User-Agent: Dalvik/2.1.0 (Linux; U; Android 12)".to_owned(),
            ],
            30,
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "Nintendo token exchange failed (HTTP {}): {}",
            response.status(),
            response.text()
        );
        let json: serde_json::Value = serde_json::from_slice(response.body())?;
        let tokens = TokenResponse {
            id_token: json_string(&json, "id_token"),
            access_token: json_string(&json, "access_token"),
            expires_in: json_i64(&json, "expires_in", 900),
        };
        anyhow::ensure!(
            !tokens.id_token.is_empty() && !tokens.access_token.is_empty(),
            "Nintendo token exchange returned incomplete credentials"
        );
        let usable = tokens.expires_in.saturating_sub(10).max(1) as u64;
        let mut cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        cache.session_token = session_token.to_owned();
        cache.tokens = tokens.clone();
        cache.token_expiry = Some(Instant::now() + Duration::from_secs(usable));
        cache.profile_access_token.clear();
        cache.profile_valid = false;
        Ok(tokens)
    }

    pub fn fetch_profile(&self, access_token: &str) -> anyhow::Result<UserProfile> {
        {
            let cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if cache.profile_valid && cache.profile_access_token == access_token {
                return Ok(cache.profile.clone());
            }
        }
        let response = self.http.get(
            PROFILE_URL,
            &[
                format!("Authorization: Bearer {access_token}"),
                "Accept: application/json".to_owned(),
                "Accept-Language: en-GB".to_owned(),
                "User-Agent: NASDKAPI; Android".to_owned(),
            ],
            30,
            4 * 1024 * 1024,
        )?;
        anyhow::ensure!(
            response.status() / 100 == 2,
            "Nintendo profile request failed: {}",
            response.text()
        );
        let json: serde_json::Value = serde_json::from_slice(response.body())?;
        let profile = UserProfile {
            id: json_string(&json, "id"),
            nickname: json_string(&json, "nickname"),
            birthday: json.get("birthday").and_then(|v| v.as_str()).unwrap_or("1995-01-01").to_owned(),
            country: json.get("country").and_then(|v| v.as_str()).unwrap_or("US").to_owned(),
            language: json.get("language").and_then(|v| v.as_str()).unwrap_or("en-GB").to_owned(),
        };
        let mut cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        cache.profile_access_token = access_token.to_owned();
        cache.profile = profile.clone();
        cache.profile_valid = true;
        Ok(profile)
    }

    pub fn clear_cached_tokens(&self) {
        *self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Cache::default();
    }
}

fn callback_parameter(url: &url::Url, name: &str) -> String {
    if let Some(value) = url
        .query_pairs()
        .find(|(key, _)| key == name)
        .map(|(_, value)| value.into_owned())
    {
        return value;
    }
    url.fragment()
        .and_then(|fragment| {
            url::form_urlencoded::parse(fragment.as_bytes())
                .find(|(key, _)| key == name)
                .map(|(_, value)| value.into_owned())
        })
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::callback_parameter;

    #[test]
    fn reads_fragment_callback_parameters() {
        let url = url::Url::parse("npf71b963c1b7b6d119://auth#state=abc&session_token_code=def")
            .expect("callback URL");
        assert_eq!(callback_parameter(&url, "state"), "abc");
        assert_eq!(callback_parameter(&url, "session_token_code"), "def");
    }
}
