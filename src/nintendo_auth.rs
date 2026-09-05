//! Nintendo Account OAuth/PKCE flow and short-lived token cache.

use crate::auth_callback::{NINTENDO_REDIRECT_URI, is_nintendo_auth_callback};
use crate::http::{DEFAULT_GET_RESPONSE_LIMIT, HttpClient};
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
        Ok(format!(
            "{AUTHORIZE_URL}?state={}&redirect_uri={}&client_id={CLIENT_ID}&scope={}&response_type=session_token_code&session_token_code_challenge={}&session_token_code_challenge_method=S256&theme=login_form",
            encode_component(&state),
            encode_component(NINTENDO_REDIRECT_URI),
            encode_component(SCOPE),
            encode_component(&challenge),
        ))
    }

    pub fn complete_login(&self, callback_url: &str) -> anyhow::Result<AuthResult> {
        let (expected_state, verifier) = {
            let cache = self.cache.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            anyhow::ensure!(
                !cache.oauth_state.is_empty() && !cache.pkce_verifier.is_empty(),
                "Nintendo sign-in session expired. Open the sign-in page again."
            );
            (cache.oauth_state.clone(), cache.pkce_verifier.clone())
        };
        anyhow::ensure!(
            is_nintendo_auth_callback(callback_url),
            "Nintendo sign-in did not return through the registered browser callback."
        );
        let returned_state = extract_parameter(callback_url, "state");
        anyhow::ensure!(
            !returned_state.is_empty() && returned_state == expected_state,
            "Nintendo sign-in callback had an invalid OAuth state."
        );
        let error = extract_parameter(callback_url, "error");
        if !error.is_empty() {
            anyhow::bail!(if error == "access_denied" {
                "Nintendo Account sign-in was cancelled.".to_owned()
            } else {
                format!("Nintendo Account sign-in failed: {error}")
            });
        }
        let code = extract_parameter(callback_url, "session_token_code");
        anyhow::ensure!(
            !code.is_empty(),
            "Nintendo sign-in callback did not include a session token code."
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
            DEFAULT_GET_RESPONSE_LIMIT,
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

fn encode_component(value: &str) -> String {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut output = String::with_capacity(value.len());
    for byte in value.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            output.push(char::from(byte));
        } else {
            output.push('%');
            output.push(char::from(HEX[usize::from(byte >> 4)]));
            output.push(char::from(HEX[usize::from(byte & 0x0f)]));
        }
    }
    output
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn decode_component(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut output = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' && index + 2 < bytes.len()
            && let (Some(high), Some(low)) = (hex_value(bytes[index + 1]), hex_value(bytes[index + 2]))
        {
            output.push((high << 4) | low);
            index += 3;
            continue;
        }
        output.push(if bytes[index] == b'+' { b' ' } else { bytes[index] });
        index += 1;
    }
    String::from_utf8_lossy(&output).into_owned()
}

fn extract_parameter(input: &str, name: &str) -> String {
    let marker = format!("{name}=");
    let mut search_from = 0;
    while let Some(relative) = input[search_from..].find(&marker) {
        let position = search_from + relative;
        if position == 0 || input.as_bytes().get(position - 1).is_some_and(|byte| matches!(byte, b'?' | b'#' | b'&')) {
            let start = position + marker.len();
            let tail = &input[start..];
            let end = tail.find(['&', '#']).unwrap_or(tail.len());
            return decode_component(&tail[..end]);
        }
        search_from = position + 1;
    }
    String::new()
}

#[cfg(test)]
mod tests {
    use super::{decode_component, encode_component, extract_parameter};

    #[test]
    fn callback_parameters_match_cpp_parser() {
        let url = "npf71b963c1b7b6d119://auth#state=abc&session_token_code=def";
        assert_eq!(extract_parameter(url, "state"), "abc");
        assert_eq!(extract_parameter(url, "session_token_code"), "def");
        assert_eq!(decode_component("a+b%20c"), "a b c");
    }

    #[test]
    fn authorize_components_use_rfc3986_unreserved_encoding() {
        assert_eq!(encode_component("openid user~x"), "openid%20user~x");
    }
}
