//! NookLink and SplatNet 2 Rich Presence enrichment.

use crate::http::HttpClient;
use serde_json::Value;
use std::collections::{BTreeMap, HashMap};
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub const ANIMAL_CROSSING_GAME_SERVICE_ID: u64 = 4_953_919_198_265_344;
pub const SPLATOON2_GAME_SERVICE_ID: u64 = 5_741_031_244_955_648;
const NOOKLINK_BASE: &str = "https://web.sd.lp1.acbaa.srv.nintendo.net";
const SPLATNET2_BASE: &str = "https://app.splatoon2.nintendo.net";
const WEB_SERVICE_USER_AGENT: &str = "Mozilla/5.0 (Linux; Android 8.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.125 Mobile Safari/537.36";
const NXAPI_WEB_SERVICE_USER_AGENT: &str = "Mozilla/5.0 (iPhone; CPU iPhone OS 15_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.3 Mobile/15E148 Safari/604.1";
const BLANCO_VERSION: &str = "2.1.1";

#[derive(Debug, Clone, Default)]
pub struct AnimalCrossingPresence {
    island_name: String,
    resident_name: String,
    image_uri: String,
    active: bool,
}
impl AnimalCrossingPresence {
    pub fn active(&self) -> bool { self.active }
    pub fn image_uri(&self) -> &str { &self.image_uri }
    pub fn format_state(&self) -> String { self.island_name.clone() }
    pub fn format_details(&self) -> String {
        match (self.resident_name.is_empty(), self.island_name.is_empty()) {
            (true, _) => self.island_name.clone(),
            (_, true) => self.resident_name.clone(),
            _ => format!("{} • {}", self.resident_name, self.island_name),
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Splatoon2Presence {
    player_name: String,
    rank_name: String,
    player_level: i64,
    star_rank: i64,
    stage_image_uri: String,
    active: bool,
}
impl Splatoon2Presence {
    pub fn active(&self) -> bool { self.active }
    pub fn stage_image_uri(&self) -> &str { &self.stage_image_uri }
    pub fn format_details(&self) -> String { self.player_name.clone() }
    pub fn format_state(&self) -> String {
        let mut parts = Vec::new();
        if self.player_level > 0 {
            parts.push(if self.star_rank > 0 { format!("Level {} (Prestige {})", self.player_level, self.star_rank) } else { format!("Level {}", self.player_level) });
        }
        if !self.rank_name.is_empty() { parts.push(self.rank_name.clone()); }
        parts.join(" • ")
    }
}

#[derive(Clone, Default)]
struct ServiceSession {
    source_token: String,
    cookie: String,
    user_id: String,
    auth_token: String,
    user_auth_attempted: bool,
    expires_at: Option<Instant>,
}

struct State {
    language: String,
    country: String,
    sessions: HashMap<String, ServiceSession>,
    shortened_urls: HashMap<String, String>,
}
impl Default for State {
    fn default() -> Self { Self { language: "en-GB".to_owned(), country: "GB".to_owned(), sessions: HashMap::new(), shortened_urls: HashMap::new() } }
}

pub struct GameServicesClient { http: HttpClient, state: Mutex<State> }

impl GameServicesClient {
    pub fn new(http: HttpClient) -> Self { Self { http, state: Mutex::new(State::default()) } }

    pub fn set_locale(&self, language: &str, country: &str) {
        let language = if language.is_empty() { "en-GB" } else { language };
        let country = if country.is_empty() { "GB" } else { country };
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if state.language == language && state.country == country { return; }
        state.language = language.to_owned();
        state.country = country.to_owned();
        state.sessions.clear();
        state.shortened_urls.clear();
    }

    pub fn clear_cache(&self) {
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        state.sessions.clear();
        state.shortened_urls.clear();
    }

    pub fn fetch_animal_crossing_presence(&self, web_service_token: &str) -> anyhow::Result<AnimalCrossingPresence> {
        if web_service_token.is_empty() { return Ok(AnimalCrossingPresence::default()); }
        let (language, country, mut session) = self.session_snapshot("nooklink", web_service_token);
        if session.cookie.is_empty() {
            let response = self.http.get(&launch_url(NOOKLINK_BASE, &language, &country), &bootstrap_headers(web_service_token, &language, WEB_SERVICE_USER_AGENT, false), 10, 4 * 1024 * 1024)?;
            if response.status() != 200 { return Ok(AnimalCrossingPresence::default()); }
            let gtoken = cookie_value(&response, "_gtoken");
            if gtoken.is_empty() { return Ok(AnimalCrossingPresence::default()); }
            session.source_token = web_service_token.to_owned();
            session.cookie = merge_cookie_header("", &response);
            if session.cookie.is_empty() { session.cookie = format!("_gtoken={gtoken}"); }
            session.expires_at = Some(Instant::now() + Duration::from_secs(90 * 60));
        }

        let users_response = self.http.get(&format!("{NOOKLINK_BASE}/api/sd/v1/users"), &nooklink_headers(&session.cookie, &language, &country), 10, 4 * 1024 * 1024)?;
        if matches!(users_response.status(), 401 | 403) { self.remove_session("nooklink"); return Ok(AnimalCrossingPresence::default()); }
        if users_response.status() != 200 { return Ok(AnimalCrossingPresence::default()); }
        session.cookie = merge_cookie_header(&session.cookie, &users_response);
        let root: Value = serde_json::from_slice(users_response.body())?;
        let Some(user) = root.get("users").and_then(Value::as_array).and_then(|users| users.first()) else { return Ok(AnimalCrossingPresence::default()); };
        let mut presence = AnimalCrossingPresence {
            resident_name: string(user, "name"),
            image_uri: first_string(user, &["image", "image_url"]),
            ..AnimalCrossingPresence::default()
        };
        session.user_id = value_string(user.get("id"));
        let mut land_id = String::new();
        if let Some(land) = user.get("land") {
            presence.island_name = string(land, "name");
            land_id = value_string(land.get("id"));
        }
        presence.active = !presence.resident_name.is_empty() || !presence.island_name.is_empty();

        if !session.user_id.is_empty() && session.auth_token.is_empty() && !session.user_auth_attempted {
            session.user_auth_attempted = true;
            let response = self.http.post_json(&format!("{NOOKLINK_BASE}/api/sd/v1/auth_token"), &serde_json::json!({"userId": session.user_id}), &nooklink_headers(&session.cookie, &language, &country), 10)?;
            session.cookie = merge_cookie_header(&session.cookie, &response);
            if response.status() / 100 == 2 {
                if let Ok(json) = serde_json::from_slice::<Value>(response.body()) { session.auth_token = string(&json, "token"); }
            }
        }

        if !session.user_id.is_empty() && !session.auth_token.is_empty() {
            let mut headers = nooklink_headers(&session.cookie, &language, &country);
            headers.push(format!("Authorization: Bearer {}", session.auth_token));
            let profile = self.http.get(&format!("{NOOKLINK_BASE}/api/sd/v1/users/{}/profile?language=en-GB", session.user_id), &headers, 10, 4 * 1024 * 1024)?;
            session.cookie = merge_cookie_header(&session.cookie, &profile);
            let mut auth_valid = true;
            if matches!(profile.status(), 401 | 403) { session.auth_token.clear(); auth_valid = false; }
            else if profile.status() == 200 {
                if let Ok(json) = serde_json::from_slice::<Value>(profile.body()) {
                    let resident = string(&json, "mPNm"); if !resident.is_empty() { presence.resident_name = resident; }
                    let island = string(&json, "landName"); if !island.is_empty() { presence.island_name = island; }
                    let image = first_string(&json, &["image", "image_url"]); if !image.is_empty() { presence.image_uri = image; }
                }
            }
            if auth_valid && !land_id.is_empty() {
                let mut headers = nooklink_headers(&session.cookie, &language, &country);
                headers.push(format!("Authorization: Bearer {}", session.auth_token));
                let island = self.http.get(&format!("{NOOKLINK_BASE}/api/sd/v1/lands/{land_id}/profile?language=en-GB"), &headers, 10, 4 * 1024 * 1024)?;
                session.cookie = merge_cookie_header(&session.cookie, &island);
                if matches!(island.status(), 401 | 403) { session.auth_token.clear(); }
                else if island.status() == 200 {
                    if let Ok(json) = serde_json::from_slice::<Value>(island.body()) {
                        let name = string(&json, "mVNm"); if !name.is_empty() { presence.island_name = name; }
                    }
                }
            }
        }

        if presence.image_uri.len() > 300 { presence.image_uri = self.shorten_image_url(&presence.image_uri); }
        if presence.image_uri.len() > 300 { presence.image_uri.clear(); }
        presence.active = !presence.resident_name.is_empty() || !presence.island_name.is_empty();
        self.store_session("nooklink", &language, &country, session);
        Ok(presence)
    }

    pub fn fetch_splatoon2_presence(&self, web_service_token: &str) -> anyhow::Result<Splatoon2Presence> {
        if web_service_token.is_empty() { return Ok(Splatoon2Presence::default()); }
        let (language, country, mut session) = self.session_snapshot("splatnet2", web_service_token);
        if session.cookie.is_empty() || session.user_id.is_empty() {
            let response = self.http.get(&launch_url(SPLATNET2_BASE, &language, &country), &bootstrap_headers(web_service_token, &language, WEB_SERVICE_USER_AGENT, false), 10, 8 * 1024 * 1024)?;
            if response.status() != 200 { return Ok(Splatoon2Presence::default()); }
            let iksm = cookie_value(&response, "iksm_session");
            let unique_id = html_attribute(&response.text(), "data-unique-id");
            if iksm.is_empty() || unique_id.is_empty() { return Ok(Splatoon2Presence::default()); }
            session.source_token = web_service_token.to_owned();
            session.cookie = format!("iksm_session={iksm}");
            session.user_id = unique_id;
            session.expires_at = Some(Instant::now() + Duration::from_secs(90 * 60));
        }
        let headers = vec![
            format!("User-Agent: {WEB_SERVICE_USER_AGENT}"), format!("Cookie: {}", session.cookie), "Accept: */*".to_owned(),
            "Accept-Language: en-GB,en-US;q=0.8".to_owned(), format!("Referer: {SPLATNET2_BASE}/home"),
            "X-Requested-With: XMLHttpRequest".to_owned(), "X-Timezone-Offset: 0".to_owned(), format!("X-Unique-Id: {}", session.user_id),
        ];
        let response = self.http.get(&format!("{SPLATNET2_BASE}/api/records"), &headers, 10, 8 * 1024 * 1024)?;
        if matches!(response.status(), 401 | 403) { self.remove_session("splatnet2"); }
        if response.status() != 200 { return Ok(Splatoon2Presence::default()); }
        let root: Value = serde_json::from_slice(response.body())?;
        let Some(player) = root.get("records").and_then(|records| records.get("player")) else { return Ok(Splatoon2Presence::default()); };
        let player_name = string(player, "nickname");
        let player_level = integer(player, "player_rank");
        let star_rank = integer(player, "star_rank");
        let stage_image_uri = player.get("weapon").map(|weapon| string(weapon, "image")).unwrap_or_default();
        let ranks = [("udemae_zones", "Zones"), ("udemae_tower", "Tower"), ("udemae_rainmaker", "Rainmaker"), ("udemae_clam", "Clams")]
            .into_iter().filter_map(|(key, short)| rank_value(player, key, short)).collect::<Vec<_>>();
        let presence = Splatoon2Presence {
            active: !player_name.is_empty() || player_level > 0 || !stage_image_uri.is_empty(),
            player_name, rank_name: ranks.join(" / "), player_level, star_rank, stage_image_uri,
        };
        self.store_session("splatnet2", &language, &country, session);
        Ok(presence)
    }

    fn shorten_image_url(&self, source: &str) -> String {
        if source.is_empty() || source.len() <= 300 { return source.to_owned(); }
        {
            let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if let Some(cached) = state.shortened_urls.get(source) { return cached.clone(); }
        }
        let encoded: String = url::form_urlencoded::byte_serialize(source.as_bytes()).collect();
        if let Ok(response) = self.http.get(&format!("https://tinyurl.com/api-create.php?url={encoded}"), &[], 5, 4096) {
            if response.status() == 200 {
                let short = response.text().trim().to_owned();
                if short.starts_with("https://") && short.len() <= 300 {
                    self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).shortened_urls.insert(source.to_owned(), short.clone());
                    return short;
                }
            }
        }
        source.to_owned()
    }

    fn session_snapshot(&self, key: &str, token: &str) -> (String, String, ServiceSession) {
        let state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let session = state.sessions.get(key).filter(|session| session.source_token == token && session.expires_at.is_some_and(|deadline| Instant::now() < deadline)).cloned().unwrap_or_default();
        (state.language.clone(), state.country.clone(), session)
    }

    fn store_session(&self, key: &str, language: &str, country: &str, session: ServiceSession) {
        let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if state.language == language && state.country == country { state.sessions.insert(key.to_owned(), session); }
    }

    fn remove_session(&self, key: &str) { self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).sessions.remove(key); }
}

fn launch_url(base: &str, language: &str, country: &str) -> String { format!("{base}/?lang={language}&na_country={country}&na_lang={language}") }

fn bootstrap_headers(token: &str, language: &str, user_agent: &str, use_language: bool) -> Vec<String> {
    vec![
        "Upgrade-Insecure-Requests: 1".to_owned(), format!("User-Agent: {user_agent}"), "x-appplatform: android".to_owned(),
        "x-appcolorscheme: DARK".to_owned(), format!("x-gamewebtoken: {token}"), "dnt: 1".to_owned(),
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8".to_owned(),
        format!("Accept-Language: {}", if use_language { language } else { "en-GB,en-US;q=0.8" }), "X-Requested-With: com.nintendo.znca".to_owned(),
    ]
}

fn nooklink_headers(cookie: &str, language: &str, country: &str) -> Vec<String> {
    vec![
        format!("User-Agent: {NXAPI_WEB_SERVICE_USER_AGENT}"), format!("Cookie: {cookie}"), "Upgrade-Insecure-Requests: 1".to_owned(), "dnt: 1".to_owned(),
        "Accept: application/json, text/plain, */*".to_owned(), format!("Accept-Language: {}", if language.is_empty() { "en-GB" } else { language }),
        format!("Origin: {NOOKLINK_BASE}"), format!("Referer: {NOOKLINK_BASE}/"), "Content-Type: application/json".to_owned(),
        format!("X-Blanco-Version: {BLANCO_VERSION}"), "x-appplatform: android".to_owned(), "x-appcolorscheme: DARK".to_owned(),
        format!("X-NACountry: {}", if country.is_empty() { "GB" } else { country }), "X-Requested-With: com.nintendo.znca".to_owned(),
    ]
}

fn cookie_value(response: &crate::http::HttpResponse, name: &str) -> String {
    response.header("set-cookie").unwrap_or_default().lines().find_map(|line| {
        let first = line.split(';').next()?.trim();
        let (key, value) = first.split_once('=')?;
        (key.trim() == name).then(|| value.trim().to_owned())
    }).unwrap_or_default()
}

fn merge_cookie_header(existing: &str, response: &crate::http::HttpResponse) -> String {
    let mut cookies = BTreeMap::<String, String>::new();
    for pair in existing.split(';').map(str::trim).filter(|pair| !pair.is_empty()) {
        if let Some((key, value)) = pair.split_once('=') { cookies.insert(key.trim().to_owned(), value.trim().to_owned()); }
    }
    for line in response.header("set-cookie").unwrap_or_default().lines() {
        if let Some((key, value)) = line.split(';').next().and_then(|pair| pair.split_once('=')) { cookies.insert(key.trim().to_owned(), value.trim().to_owned()); }
    }
    cookies.into_iter().map(|(key, value)| format!("{key}={value}")).collect::<Vec<_>>().join("; ")
}

fn html_attribute(body: &str, name: &str) -> String {
    for quote in ['"', '\''] {
        let marker = format!("{name}={quote}");
        if let Some(start) = body.find(&marker) {
            let value_start = start + marker.len();
            if let Some(end) = body[value_start..].find(quote) { return body[value_start..value_start + end].to_owned(); }
        }
    }
    String::new()
}

fn rank_value(player: &Value, key: &str, short_name: &str) -> Option<String> {
    let rank = player.get(key)?.as_object()?;
    if rank.get("is_x").and_then(Value::as_bool).unwrap_or(false) { return Some(format!("{short_name} X")); }
    let mut name = rank.get("name").and_then(Value::as_str)?.to_owned();
    if name == "S+" {
        if let Some(number) = rank.get("s_plus_number").and_then(Value::as_i64).filter(|number| *number >= 0) { name.push_str(&number.to_string()); }
    }
    Some(format!("{short_name} {name}"))
}

fn string(value: &Value, key: &str) -> String { value.get(key).and_then(Value::as_str).unwrap_or_default().to_owned() }
fn first_string(value: &Value, keys: &[&str]) -> String { keys.iter().map(|key| string(value, key)).find(|value| !value.is_empty()).unwrap_or_default() }
fn integer(value: &Value, key: &str) -> i64 { value.get(key).and_then(Value::as_i64).unwrap_or_default() }
fn value_string(value: Option<&Value>) -> String { value.and_then(|value| value.as_str().map(ToOwned::to_owned).or_else(|| value.as_u64().map(|number| number.to_string())).or_else(|| value.as_i64().map(|number| number.to_string()))).unwrap_or_default() }
