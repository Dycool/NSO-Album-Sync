//! SplatNet 3 Rich Presence enrichment.

use crate::http::HttpClient;
use serde_json::{Value, json};
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub const SPLATNET3_GAME_SERVICE_ID: u64 = 4_834_290_508_791_808;
pub const SPLATNET3_GAME_SERVICE_ID_ALT: u64 = 4_834_290_530_795_520;
const BASE_URL: &str = "https://api.lp1.av5ja.srv.nintendo.net";
const USER_AGENT: &str = "Mozilla/5.0 (Linux; Android 8.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.125 Mobile Safari/537.36";
const RESOURCE_PREFIX: &str = "https://api.lp1.av5ja.srv.nintendo.net/resources/prod/";
const MIRROR_PREFIX: &str = "https://splatoon3.ink/assets/splatnet/";
const WEB_VIEW_VERSION: &str = "10.0.0-4787c271";
const HISTORY_RECORD_QUERY: &str = "a654ecc80161a7ca5c38761c1d9e502d405eae764e2d343618b9c74b1dc0a80f";

#[derive(Debug, Clone, Default)]
pub struct SplatNetPresence {
    player_name: String,
    player_id: String,
    title: String,
    weapon_name: String,
    rank_name: String,
    player_level: i64,
    stage_image_uri: String,
    active: bool,
}
impl SplatNetPresence {
    pub fn active(&self) -> bool { self.active }
    pub fn player_id(&self) -> &str { &self.player_id }
    pub fn weapon_name(&self) -> &str { &self.weapon_name }
    pub fn stage_image_uri(&self) -> &str { &self.stage_image_uri }
    pub fn format_details(&self) -> String {
        if self.player_name.is_empty() { return self.title.clone(); }
        if self.title.is_empty() { return self.player_name.clone(); }
        format!("{} • {}", self.player_name, self.title)
    }
    pub fn format_state(&self) -> String {
        let mut parts = Vec::new();
        if self.player_level > 0 { parts.push(format!("Level {}", self.player_level)); }
        if !self.rank_name.is_empty() { parts.push(format!("Rank {}", self.rank_name)); }
        if parts.is_empty() && !self.title.is_empty() { self.title.clone() } else { parts.join(" • ") }
    }
}

struct State {
    source_token: String,
    bullet_token: String,
    account_language: String,
    account_country: String,
    language: String,
    bullet_expires_at: Option<Instant>,
}
impl Default for State {
    fn default() -> Self {
        Self { source_token: String::new(), bullet_token: String::new(), account_language: "en-GB".to_owned(), account_country: "GB".to_owned(), language: "en-GB".to_owned(), bullet_expires_at: None }
    }
}

pub struct SplatNetClient { http: HttpClient, state: Mutex<State> }

impl SplatNetClient {
    pub fn new(http: HttpClient) -> Self { Self { http, state: Mutex::new(State::default()) } }

    pub fn set_locale(&self, language: &str, country: &str) {
        let language = if language.is_empty() { "en-GB" } else { language };
        let country = if country.is_empty() { "GB" } else { country };
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if state.account_language == language && state.account_country == country { return; }
        state.account_language = language.to_owned();
        state.account_country = country.to_owned();
        state.language = supported_language(language).to_owned();
        state.source_token.clear();
        state.bullet_token.clear();
        state.bullet_expires_at = None;
    }

    pub fn clear_cache(&self) {
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.source_token.clear();
        state.bullet_token.clear();
        state.language = supported_language(&state.account_language).to_owned();
        state.bullet_expires_at = None;
    }

    pub fn fetch_presence(&self, web_service_token: &str) -> anyhow::Result<SplatNetPresence> {
        if web_service_token.is_empty() { return Ok(SplatNetPresence::default()); }
        let bullet = self.ensure_bullet_token(web_service_token)?;
        if bullet.is_empty() { return Ok(SplatNetPresence::default()); }
        let language = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).language.clone();
        let body = json!({"variables": {}, "extensions": {"persistedQuery": {"version": 1, "sha256Hash": HISTORY_RECORD_QUERY}}});
        let response = self.http.post_json(&format!("{BASE_URL}/api/graphql"), &body, &[
            format!("User-Agent: {USER_AGENT}"),
            "Accept: */*".to_owned(),
            format!("Referer: {BASE_URL}/"),
            "X-Requested-With: XMLHttpRequest".to_owned(),
            format!("Authorization: Bearer {bullet}"),
            format!("X-Web-View-Ver: {WEB_VIEW_VERSION}"),
            format!("Accept-Language: {language}"),
        ], 10)?;
        if matches!(response.status(), 401 | 403) { self.clear_cache(); }
        if response.status() != 200 { return Ok(SplatNetPresence::default()); }
        let root: Value = serde_json::from_slice(response.body())?;
        let Some(data) = root.get("data") else { return Ok(SplatNetPresence::default()); };
        let Some(player) = data.get("currentPlayer") else { return Ok(SplatNetPresence::default()); };
        let player_name = string(player, "name");
        let player_id = string(player, "nameId");
        let title = string(player, "byname");
        let (weapon_name, stage_image_uri) = player.get("weapon").map(|weapon| {
            let name = string(weapon, "name");
            let image = weapon.get("image").map(|image| string(image, "url")).map(|url| discord_weapon_image_url(&url)).unwrap_or_default();
            (name, image)
        }).unwrap_or_default();
        let history = data.get("playHistory").unwrap_or(&Value::Null);
        let player_level = history.get("rank").and_then(Value::as_i64).unwrap_or_default();
        let rank_name = history.get("udemae").map(|rank| rank.as_str().map(ToOwned::to_owned).or_else(|| rank.get("name").and_then(Value::as_str).map(ToOwned::to_owned)).unwrap_or_default()).unwrap_or_default();
        Ok(SplatNetPresence {
            active: !player_name.is_empty() || !weapon_name.is_empty() || !title.is_empty(),
            player_name,
            player_id,
            title,
            weapon_name,
            rank_name,
            player_level,
            stage_image_uri,
        })
    }

    fn ensure_bullet_token(&self, web_service_token: &str) -> anyhow::Result<String> {
        let (language, country) = {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.source_token == web_service_token && !state.bullet_token.is_empty() && state.bullet_expires_at.is_some_and(|deadline| Instant::now() < deadline) {
                return Ok(state.bullet_token.clone());
            }
            (state.account_language.clone(), state.account_country.clone())
        };
        let api_language = supported_language(&language);
        let launch = format!("{BASE_URL}/?lang={language}&na_country={country}&na_lang={language}");
        let bootstrap = self.http.get(&launch, &bootstrap_headers(web_service_token), 10, 8 * 1024 * 1024)?;
        if bootstrap.status() != 200 { return Ok(String::new()); }
        let response = self.http.post_text(&format!("{BASE_URL}/api/bullet_tokens"), "", &[
            format!("User-Agent: {USER_AGENT}"), "Accept: */*".to_owned(), format!("Referer: {BASE_URL}/"),
            "X-Requested-With: XMLHttpRequest".to_owned(), format!("X-Web-View-Ver: {WEB_VIEW_VERSION}"),
            format!("X-NACOUNTRY: {country}"), format!("Accept-Language: {api_language}"), format!("X-GameWebToken: {web_service_token}"),
        ], "application/json", 10, 4 * 1024 * 1024)?;
        if response.status() != 201 { return Ok(String::new()); }
        let json: Value = serde_json::from_slice(response.body())?;
        let bullet = string(&json, "bulletToken");
        if bullet.is_empty() { return Ok(String::new()); }
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if state.account_language != language || state.account_country != country { return Ok(String::new()); }
        state.source_token = web_service_token.to_owned();
        state.bullet_token = bullet.clone();
        state.language = json.get("lang").and_then(Value::as_str).unwrap_or(api_language).to_owned();
        state.bullet_expires_at = Some(Instant::now() + Duration::from_secs(100 * 60));
        Ok(bullet)
    }
}

fn bootstrap_headers(token: &str) -> Vec<String> {
    vec![
        "Upgrade-Insecure-Requests: 1".to_owned(), format!("User-Agent: {USER_AGENT}"), "x-appplatform: android".to_owned(),
        "x-appcolorscheme: DARK".to_owned(), format!("x-gamewebtoken: {token}"), "dnt: 1".to_owned(),
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8".to_owned(),
        "Accept-Language: en-GB,en-US;q=0.8".to_owned(), "X-Requested-With: com.nintendo.znca".to_owned(),
    ]
}

fn supported_language(language: &str) -> &str {
    const LANGUAGES: &[&str] = &["de-DE", "en-GB", "en-US", "es-ES", "es-MX", "fr-CA", "fr-FR", "it-IT", "ja-JP", "ko-KR", "nl-NL", "ru-RU", "zh-CN", "zh-TW"];
    if LANGUAGES.contains(&language) { language } else { "en-GB" }
}

fn discord_weapon_image_url(signed_url: &str) -> String {
    if let Some(rest) = signed_url.strip_prefix(RESOURCE_PREFIX) {
        let resource = rest.split(['?', '#']).next().unwrap_or_default();
        if resource.is_empty() || resource.contains("..") || resource.chars().any(char::is_whitespace) { return String::new(); }
        let mirrored = format!("{MIRROR_PREFIX}{resource}");
        if mirrored.len() <= 300 { mirrored } else { String::new() }
    } else if signed_url.len() <= 300 { signed_url.to_owned() } else { String::new() }
}

fn string(value: &Value, key: &str) -> String { value.get(key).and_then(Value::as_str).unwrap_or_default().to_owned() }

#[cfg(test)]
mod tests {
    use super::discord_weapon_image_url;

    #[test]
    fn signed_weapon_url_uses_reference_mirror_shape() {
        let source = "https://api.lp1.av5ja.srv.nintendo.net/resources/prod/weapon.png?Signature=long";
        assert_eq!(discord_weapon_image_url(source), "https://splatoon3.ink/assets/splatnet/weapon.png");
    }
}
