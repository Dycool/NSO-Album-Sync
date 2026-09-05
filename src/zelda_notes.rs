//! Zelda Notes live map synchronization and Discord-ready location overlay.

use crate::http::HttpClient;
use crate::sse::SseClient;
use crate::util::random_alphanumeric;
use crate::zelda_regions::{LocationResult, Vector3, ZeldaGame, ZeldaLayer, resolve_botw_location_3d, resolve_poi_artwork, resolve_region_artwork, resolve_totk_location_3d};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

pub const ZELDA_NOTES_GAME_SERVICE_ID: u64 = 5_935_781_783_175_168;
pub const ZELDA_NOTES_GAME_SERVICE_ID_ALT: u64 = 4_974_384_874_151_936;
pub const ZELDA_NOTES_BOTW_TITLE_ID: &str = "01007ef00011e000";
pub const ZELDA_NOTES_TOTK_TITLE_ID: &str = "0100f2c0115b6000";
const BASE_URL: &str = "https://api.lp1.87abc152.srv.nintendo.net";
const USER_AGENT: &str = "Mozilla/5.0 (Linux; Android 10; Build/QP1A.190711.020; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/80.0.3987.162 Mobile Safari/537.36 com.nintendo.znca/3.4.1";
const FALLBACK_START_ACTION: &str = "70133dd2eb7d5126fda8aa9c8ff56d5a0376deadba";
const FALLBACK_ACK_ACTION: &str = "400d043452ef637b91e45e5861062b9677aa6fbf22";
const FALLBACK_DEPLOYMENT_ID: &str = "783666f6880ab3979bdc7b15f8ad24f544e472e5";
const LIVE_FRESHNESS: Duration = Duration::from_secs(30);
const LIVE_RECONNECT_IDLE: Duration = Duration::from_secs(10 * 60);

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ZeldaNotesPresence {
    title_name: String,
    profile_summary: String,
    stage_image_uri: String,
    stage_name: String,
    avatar_url: String,
    active: bool,
}
impl ZeldaNotesPresence {
    pub fn active(&self) -> bool { self.active }
    pub fn format_state(&self) -> &str { &self.title_name }
    pub fn format_details(&self) -> &str { &self.profile_summary }
    pub fn stage_image_uri(&self) -> &str { &self.stage_image_uri }
    pub fn stage_name(&self) -> &str { &self.stage_name }
    pub fn avatar_url(&self) -> &str { &self.avatar_url }
}

#[derive(Debug, Clone)]
struct LiveState {
    game: ZeldaGame,
    layer: ZeldaLayer,
    position: Vector3,
    received_at: Instant,
    synchronized: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum LiveMessageType { Unknown, Open, MapSyncStartAck, MapSyncPlayerInfo }

struct LiveMessage {
    message_type: LiveMessageType,
    game_session_id: String,
    needs_ack: bool,
    request_id: String,
    updates_live_state: bool,
    state: Option<LiveState>,
    valid: bool,
}

#[derive(Clone)]
struct MapPlace {
    uid: i64,
    subcategory: String,
    message_label: String,
    layer: ZeldaLayer,
    position: Vector3,
}

#[derive(Clone, Default)]
struct WebMetadata {
    start_action: String,
    end_action: String,
    ack_action: String,
    deployment_id: String,
    custom_avatar_url: String,
    labels: HashMap<String, String>,
    places: Vec<MapPlace>,
}
impl WebMetadata {
    fn protocol_ready(&self) -> bool {
        !self.start_action.is_empty() && !self.end_action.is_empty() && !self.ack_action.is_empty()
    }
}

struct ServerActionContext<'a> {
    http: &'a HttpClient,
    metadata: &'a WebMetadata,
    game: ZeldaGame,
    session: &'a str,
    language: &'a str,
    country: &'a str,
}

impl ServerActionContext<'_> {
    fn send(&self, action: &str, arguments: Vec<Value>) -> anyhow::Result<bool> {
        if action.is_empty() { return Ok(false); }
        let page_url = route_url(self.game);
        let mut headers = authenticated_headers(self.session, self.language, self.country, "text/x-component");
        headers.extend([
            format!("Next-Action: {action}"),
            format!("Next-Router-State-Tree: {}", router_state_tree(self.game)),
            format!("Origin: {BASE_URL}"),
            format!("Referer: {page_url}"),
        ]);
        if !self.metadata.deployment_id.is_empty() {
            headers.push(format!("X-Deployment-Id: {}", self.metadata.deployment_id));
        }
        let response = self.http.post_text(
            &page_url,
            &Value::Array(arguments).to_string(),
            &headers,
            "text/plain;charset=UTF-8",
            5,
            4 * 1024 * 1024,
        )?;
        let text = response.text();
        Ok(response.status() / 100 == 2
            && (text.is_empty() || text.contains("\"isSuccess\":true") || !text.contains("isSuccess")))
    }
}

#[derive(Debug, Clone, Default)]
struct ResolvedLocation {
    layer: ZeldaLayer,
    region: String,
    poi: String,
    stage_image_uri: String,
    subcategory: String,
    poi_uid: i64,
    poi_distance: f64,
    valid: bool,
    at_poi: bool,
    near_poi: bool,
}

struct SessionState {
    language: String,
    country: String,
    source_token: String,
    session_cookie: String,
    expires_at: Option<Instant>,
    latest_web_token: String,
}
impl Default for SessionState {
    fn default() -> Self { Self { language: "en-GB".to_owned(), country: "GB".to_owned(), source_token: String::new(), session_cookie: String::new(), expires_at: None, latest_web_token: String::new() } }
}

pub struct ZeldaNotesClient {
    http: HttpClient,
    session: Mutex<SessionState>,
    live_presence: Mutex<ZeldaNotesPresence>,
    live_game: Mutex<ZeldaGame>,
    live_stop: AtomicBool,
    live_thread: Mutex<Option<JoinHandle<()>>>,
    refresh_callback: Mutex<Option<Arc<dyn Fn() + Send + Sync>>>,
}

impl ZeldaNotesClient {
    pub fn new(http: HttpClient) -> Self {
        Self {
            http, session: Mutex::new(SessionState::default()), live_presence: Mutex::new(ZeldaNotesPresence::default()),
            live_game: Mutex::new(ZeldaGame::Unknown), live_stop: AtomicBool::new(false), live_thread: Mutex::new(None), refresh_callback: Mutex::new(None),
        }
    }

    pub fn set_refresh_callback(&self, callback: Option<Arc<dyn Fn() + Send + Sync>>) {
        *self.refresh_callback.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = callback;
    }

    pub fn set_locale(self: &Arc<Self>, language: &str, country: &str) {
        let language = if language.is_empty() { "en-GB" } else { language };
        let country = if country.is_empty() { "GB" } else { country };
        let active_game = *self.live_game.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        {
            let mut session = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if session.language == language && session.country == country { return; }
            session.language = language.to_owned(); session.country = country.to_owned();
            session.source_token.clear(); session.session_cookie.clear(); session.expires_at = None;
        }
        if active_game != ZeldaGame::Unknown { self.stop_live_session(); self.set_active_game(active_game); }
    }

    pub fn clear_cache(&self) {
        self.stop_live_session();
        let mut session = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        session.source_token.clear(); session.session_cookie.clear(); session.expires_at = None; session.latest_web_token.clear();
    }

    pub fn fetch_presence(&self, web_service_token: &str) -> anyhow::Result<ZeldaNotesPresence> {
        if web_service_token.is_empty() { return Ok(ZeldaNotesPresence::default()); }
        self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).latest_web_token = web_service_token.to_owned();
        let _ = self.ensure_session(web_service_token);
        Ok(ZeldaNotesPresence::default())
    }

    pub fn set_active_game(self: &Arc<Self>, game: ZeldaGame) {
        if game == ZeldaGame::Unknown { self.stop_live_session(); return; }
        let token = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).latest_web_token.clone();
        if token.is_empty() { return; }
        let current = *self.live_game.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let running = self.live_thread.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).is_some();
        if current == game && running && !self.live_stop.load(Ordering::Acquire) { return; }
        self.stop_live_session();
        self.live_stop.store(false, Ordering::Release);
        *self.live_game.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = game;
        let client = Arc::clone(self);
        let handle = std::thread::spawn(move || client.run_live_session(game, token));
        *self.live_thread.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(handle);
    }

    pub fn stop_live_session(&self) {
        self.live_stop.store(true, Ordering::Release);
        let handle = self.live_thread.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).take();
        if let Some(handle) = handle { let _ = handle.join(); }
        *self.live_game.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = ZeldaGame::Unknown;
        self.publish(ZeldaNotesPresence::default());
    }

    pub fn live_presence(&self) -> ZeldaNotesPresence { self.live_presence.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).clone() }

    fn ensure_session(&self, token: &str) -> anyhow::Result<String> {
        let (language, country) = {
            let session = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if session.source_token == token && !session.session_cookie.is_empty() && session.expires_at.is_some_and(|deadline| Instant::now() < deadline) { return Ok(session.session_cookie.clone()); }
            (session.language.clone(), session.country.clone())
        };
        let url = format!("{BASE_URL}/title-select?lang={language}&na_country={country}&na_lang={language}");
        let response = self.http.get(&url, &bootstrap_headers(token, &language, &country), 10, 8 * 1024 * 1024)?;
        anyhow::ensure!(matches!(response.status() / 100, 2 | 3), "Zelda Notes session bootstrap failed (HTTP {})", response.status());
        let cookie = session_cookie(&response);
        anyhow::ensure!(!cookie.is_empty(), "Zelda Notes session bootstrap returned no session cookie");
        let mut session = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        anyhow::ensure!(session.language == language && session.country == country, "Zelda Notes locale changed during bootstrap");
        session.source_token = token.to_owned(); session.session_cookie = cookie.clone(); session.expires_at = Some(Instant::now() + Duration::from_secs(90 * 60));
        Ok(cookie)
    }

    fn run_live_session(self: Arc<Self>, game: ZeldaGame, token: String) {
        let mut backoff = 2_u64;
        let mut metadata = WebMetadata::default();
        while !self.live_stop.load(Ordering::Acquire) {
            let porter_id = random_alphanumeric(5);
            let result = (|| -> anyhow::Result<bool> {
                let session_cookie = self.ensure_session(&token)?;
                let (language, country) = {
                    let session = self.session.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                    (session.language.clone(), session.country.clone())
                };
                if !metadata.protocol_ready() {
                    metadata = discover_web_metadata(&self.http, game, &session_cookie, &language, &country)?;
                }
                anyhow::ensure!(metadata.protocol_ready(), "Zelda Notes map-sync actions unavailable");
                let action_context = ServerActionContext {
                    http: &self.http,
                    metadata: &metadata,
                    game,
                    session: &session_cookie,
                    language: &language,
                    country: &country,
                };
                let url = format!("{BASE_URL}/continuous-connection/sse?gameId={}&porterSessionId={porter_id}", game_id(game));
                let mut headers = authenticated_headers(&session_cookie, &language, &country, "text/event-stream");
                headers.extend(["Cache-Control: no-cache".to_owned(), "Pragma: no-cache".to_owned(), format!("Referer: {}", route_url(game))]);
                let sse = SseClient::new(self.http.proxy_url());
                let mut game_session_id = String::new();
                let mut previous = ResolvedLocation::default();
                let last_message = std::cell::Cell::new(Instant::now());
                let mut protocol_failure = false;
                let response = sse.stream(&url, &headers, |event| {
                    if self.live_stop.load(Ordering::Acquire) { return false; }
                    let received_at = Instant::now();
                    let message = decode_live_message(event.data(), game, received_at);
                    if !message.valid { return true; }
                    last_message.set(received_at);
                    if !message.game_session_id.is_empty() { game_session_id = message.game_session_id.clone(); }
                    if message.needs_ack && !message.request_id.is_empty() {
                        let _ = action_context.send(&metadata.ack_action, vec![Value::String(message.request_id)]);
                    }
                    if message.message_type == LiveMessageType::Open {
                        if !game_session_id.is_empty() {
                            let _ = action_context.send(
                                &metadata.end_action,
                                vec![
                                    Value::String(game_id(game).to_owned()),
                                    Value::String(porter_id.clone()),
                                    Value::String(game_session_id.clone()),
                                ],
                            );
                            game_session_id.clear();
                        }
                        let started = action_context.send(
                            &metadata.start_action,
                            vec![
                                Value::String(game_id(game).to_owned()),
                                Value::String(porter_id.clone()),
                                Value::String("complete-guide".to_owned()),
                            ],
                        ).unwrap_or(false);
                        if !started { protocol_failure = true; return false; }
                    } else if message.updates_live_state
                        && let Some(state) = message.state
                    {
                        if !state.synchronized || state.received_at.elapsed() >= LIVE_FRESHNESS {
                            previous = ResolvedLocation::default();
                            self.publish(ZeldaNotesPresence::default());
                        } else {
                            let location = resolve_location(&metadata, &state, &previous);
                            previous = location.clone();
                            self.publish(format_presence(&metadata, &state, &location));
                        }
                    }
                    true
                }, || {
                    let idle = last_message.get().elapsed();
                    if idle >= LIVE_FRESHNESS { self.publish(ZeldaNotesPresence::default()); }
                    self.live_stop.load(Ordering::Acquire) || idle >= LIVE_RECONNECT_IDLE
                }, 20, 1024 * 1024)?;
                if !game_session_id.is_empty() {
                    let _ = action_context.send(
                        &metadata.end_action,
                        vec![
                            Value::String(game_id(game).to_owned()),
                            Value::String(porter_id),
                            Value::String(game_session_id),
                        ],
                    );
                }
                self.publish(ZeldaNotesPresence::default());
                if response.status() != 0 && response.status() / 100 != 2 { protocol_failure = true; }
                Ok(protocol_failure)
            })();
            if self.live_stop.load(Ordering::Acquire) { break; }
            match result {
                Ok(protocol_failure) => { if protocol_failure { metadata = WebMetadata::default(); } backoff = 2; }
                Err(_) => { self.publish(ZeldaNotesPresence::default()); backoff = (backoff * 2).clamp(2, 30); }
            }
            for _ in 0..backoff * 10 {
                if self.live_stop.load(Ordering::Acquire) { break; }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }

    fn publish(&self, presence: ZeldaNotesPresence) {
        let changed = {
            let mut current = self.live_presence.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            let changed = current.active != presence.active
                || current.title_name != presence.title_name
                || current.profile_summary != presence.profile_summary
                || current.stage_image_uri != presence.stage_image_uri;
            if changed { *current = presence; }
            changed
        };
        if changed
            && let Some(callback) = self.refresh_callback.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).clone()
        {
            callback();
        }
    }
}

pub fn game_for_presence(title_id: &str, game_name: &str) -> ZeldaGame {
    match title_id {
        ZELDA_NOTES_BOTW_TITLE_ID => ZeldaGame::BreathOfTheWild,
        ZELDA_NOTES_TOTK_TITLE_ID => ZeldaGame::TearsOfTheKingdom,
        _ if game_name.contains("Breath of the Wild") || game_name.contains("ブレス オブ ザ ワイルド") => ZeldaGame::BreathOfTheWild,
        _ if game_name.contains("Tears of the Kingdom") || game_name.contains("ティアーズ オブ ザ キングダム") => ZeldaGame::TearsOfTheKingdom,
        _ => ZeldaGame::Unknown,
    }
}

fn decode_live_message(payload: &str, game: ZeldaGame, received_at: Instant) -> LiveMessage {
    let Ok(root) = serde_json::from_str::<Value>(payload) else { return invalid_message(); };
    let Some(object) = root.as_object() else { return invalid_message(); };
    let message_name = object.get("messageType").and_then(Value::as_str).unwrap_or_default();
    if message_name.is_empty() { return invalid_message(); }
    let message_type = match message_name { "open" => LiveMessageType::Open, "map_sync_start_ack" => LiveMessageType::MapSyncStartAck, "map_sync_player_info" => LiveMessageType::MapSyncPlayerInfo, _ => LiveMessageType::Unknown };
    let mut message = LiveMessage {
        message_type, game_session_id: object.get("gameSessionId").and_then(Value::as_str).unwrap_or_default().to_owned(),
        needs_ack: object.get("needsAck").and_then(Value::as_bool).unwrap_or(false), request_id: object.get("messageRequestId").and_then(Value::as_str).unwrap_or_default().to_owned(),
        updates_live_state: false, state: None, valid: true,
    };
    if message_type != LiveMessageType::MapSyncPlayerInfo { return message; }
    message.updates_live_state = true;
    let position = read_vector3(object.get("playerPos"));
    let front = read_vector3(object.get("playerFront"));
    let layer = match game {
        ZeldaGame::TearsOfTheKingdom => layer_from_wire(object.get("playerLayer").and_then(Value::as_str).unwrap_or_default()),
        ZeldaGame::BreathOfTheWild => ZeldaLayer::Ground,
        ZeldaGame::Unknown => ZeldaLayer::Unknown,
    };
    let synchronized = position.is_some()
        && front.is_some()
        && match game {
            ZeldaGame::TearsOfTheKingdom => layer != ZeldaLayer::Unknown,
            ZeldaGame::BreathOfTheWild => true,
            ZeldaGame::Unknown => false,
        };
    message.state = Some(LiveState {
        game,
        layer,
        position: position.unwrap_or_default(),
        received_at,
        synchronized,
    });
    message
}

fn invalid_message() -> LiveMessage { LiveMessage { message_type: LiveMessageType::Unknown, game_session_id: String::new(), needs_ack: false, request_id: String::new(), updates_live_state: false, state: None, valid: false } }

fn read_vector3(value: Option<&Value>) -> Option<Vector3> {
    let values = value?.as_array()?;
    if values.len() < 3 { return None; }
    Vector3::new(values[0].as_f64()?, values[1].as_f64()?, values[2].as_f64()?)
}

fn layer_from_wire(value: &str) -> ZeldaLayer { match value { "Ground" => ZeldaLayer::Ground, "Sky" => ZeldaLayer::Sky, "Underground" => ZeldaLayer::Underground, _ => ZeldaLayer::Unknown } }

fn discover_web_metadata(http: &HttpClient, game: ZeldaGame, session: &str, language: &str, country: &str) -> anyhow::Result<WebMetadata> {
    let mut metadata = WebMetadata::default();
    let page_url = route_url(game);
    if page_url.is_empty() { return Ok(metadata); }
    let page = http.get(&page_url, &authenticated_headers(session, language, country, "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"), 12, 4 * 1024 * 1024)?;
    if page.status() / 100 != 2 { return Ok(metadata); }
    let html = page.text();
    metadata.start_action = find_server_reference(&html, "sendMapSyncStartAction").unwrap_or_default();
    metadata.end_action = find_server_reference(&html, "sendMapSyncEndAction").unwrap_or_default();
    metadata.ack_action = find_server_reference(&html, "sendAckAction").unwrap_or_default();
    metadata.deployment_id = page.header("x-deployment-id").unwrap_or_default().to_owned();
    let scripts = extract_script_urls(&html);
    for script_url in scripts.into_iter().take(64) {
        if metadata.deployment_id.is_empty()
            && let Some(value) = deployment_id_from_url(&script_url)
        {
            metadata.deployment_id = value;
        }
        let Ok(response) = http.get(&script_url, &authenticated_headers(session, language, country, "application/javascript,text/javascript,*/*;q=0.8"), 12, 2 * 1024 * 1024) else { continue; };
        if response.status() / 100 != 2 { continue; }
        let script = response.text();
        if metadata.start_action.is_empty() {
            metadata.start_action = find_server_reference(&script, "sendMapSyncStartAction").unwrap_or_default();
        }
        if metadata.end_action.is_empty() {
            metadata.end_action = find_server_reference(&script, "sendMapSyncEndAction").unwrap_or_default();
        }
        if metadata.ack_action.is_empty() {
            metadata.ack_action = find_server_reference(&script, "sendAckAction").unwrap_or_default();
        }
        if metadata.places.is_empty() { metadata.places = extract_map_dataset(&script, game).unwrap_or_default(); }
    }
    if metadata.start_action.is_empty() { metadata.start_action = FALLBACK_START_ACTION.to_owned(); }
    if metadata.end_action.is_empty() { metadata.end_action = FALLBACK_START_ACTION.to_owned(); }
    if metadata.ack_action.is_empty() { metadata.ack_action = FALLBACK_ACK_ACTION.to_owned(); }
    if metadata.deployment_id.is_empty() { metadata.deployment_id = FALLBACK_DEPLOYMENT_ID.to_owned(); }

    metadata.custom_avatar_url = find_ugc_avatar(&html);
    if metadata.custom_avatar_url.is_empty() {
        let profile_url = format!("{BASE_URL}/{}/profile", short_name(game));
        let profile_page = http.get(
            &profile_url,
            &authenticated_headers(session, language, country, "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
            8,
            4 * 1024 * 1024,
        )?;
        if profile_page.status() == 200 {
            metadata.custom_avatar_url = find_ugc_avatar(&profile_page.text());
        }
    }
    metadata.labels = fetch_labels(http, session, language, country).unwrap_or_default();
    Ok(metadata)
}

fn find_server_reference(source: &str, action_name: &str) -> Option<String> {
    let mut search_from = 0;
    while let Some(relative) = source[search_from..].find(action_name) {
        let name_at = search_from + relative;
        let begin = name_at.saturating_sub(512);
        let before = &source[begin..name_at];
        let mut best = None;
        let mut pieces = before.split('"');
        while let Some(_before_quote) = pieces.next() {
            let Some(candidate) = pieces.next() else { break; };
            if candidate.len() == 40 && candidate.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                best = Some(candidate.to_owned());
            }
        }
        if best.is_some() { return best; }
        search_from = name_at + action_name.len();
    }
    None
}

fn extract_script_urls(html: &str) -> Vec<String> {
    let mut output = Vec::new();
    let mut cursor = 0;
    while let Some(relative) = html[cursor..].find("src=") {
        cursor += relative + 4;
        while html[cursor..].chars().next().is_some_and(char::is_whitespace) {
            cursor += html[cursor..].chars().next().map_or(0, char::len_utf8);
        }
        let rest = &html[cursor..];
        let Some(quote) = rest.chars().next().filter(|value| matches!(value, '"' | '\'')) else { continue; };
        let body = &rest[quote.len_utf8()..];
        let Some(end) = body.find(quote) else { break; };
        let mut url = html_unescape(&body[..end]);
        cursor += quote.len_utf8() + end + quote.len_utf8();
        if !url.contains("/_next/") || !url.contains(".js") { continue; }
        if url.starts_with('/') { url = format!("{BASE_URL}{url}"); }
        if !url.starts_with(BASE_URL) { continue; }
        if !output.contains(&url) { output.push(url); }
        if output.len() >= 64 { break; }
    }
    output
}

fn deployment_id_from_url(url: &str) -> Option<String> {
    let start = url.find("dpl=")? + 4;
    Some(url[start..].split('&').next().unwrap_or_default().to_owned()).filter(|value| !value.is_empty())
}

fn extract_map_dataset(script: &str, game: ZeldaGame) -> Option<Vec<MapPlace>> {
    let marker = "JSON.parse('";
    let mut cursor = 0;
    while let Some(relative) = script[cursor..].find(marker) {
        let start = cursor + relative + marker.len();
        let bytes = script.as_bytes();
        let mut end = start;
        let mut escaped = false;
        while end < bytes.len() {
            let byte = bytes[end];
            if escaped { escaped = false; end += 1; continue; }
            if byte == b'\\' { escaped = true; end += 1; continue; }
            if byte == b'\'' { break; }
            end += 1;
        }
        if end >= bytes.len() { break; }
        let decoded = decode_js_single_quoted(&script[start..end]);
        if decoded.starts_with("[{\"uid\":")
            && let Some(places) = parse_map_array(&decoded, game)
        {
            return Some(places);
        }
        cursor = end + 1;
    }
    None
}

fn decode_js_single_quoted(value: &str) -> String {
    let mut output = String::with_capacity(value.len());
    let mut chars = value.chars();
    while let Some(character) = chars.next() {
        if character != '\\' { output.push(character); continue; }
        let Some(next) = chars.next() else { output.push('\\'); break; };
        match next { '\\' => output.push('\\'), '\'' => output.push('\''), '"' => output.push('"'), '/' => output.push('/'), 'b' => output.push('\u{0008}'), 'f' => output.push('\u{000C}'), 'n' => output.push('\n'), 'r' => output.push('\r'), 't' => output.push('\t'), other => { output.push('\\'); output.push(other); } }
    }
    output
}

fn parse_map_array(text: &str, game: ZeldaGame) -> Option<Vec<MapPlace>> {
    let root = serde_json::from_str::<Value>(text).ok()?;
    let array = root.as_array()?;
    if array.is_empty() { return None; }
    let looks_like_map = array.iter().any(|item| item.get("uid").is_some() && item.get("viewCategory").is_some() && item.get("coordinates").is_some());
    if !looks_like_map { return None; }
    let looks_totk = array.iter().any(|item| item.get("layer").is_some());
    if (game == ZeldaGame::TearsOfTheKingdom) != looks_totk { return None; }
    let mut output = Vec::new();
    for item in array {
        if item.get("viewCategory").and_then(Value::as_str) != Some("Location") { continue; }
        let subcategory = item.get("viewSubCategory").and_then(Value::as_str).unwrap_or_default();
        if !wanted_subcategory(game, subcategory) { continue; }
        let Some(position) = read_vector3(item.get("coordinates")) else { continue; };
        let uid = item.get("uid").and_then(Value::as_i64).or_else(|| item.get("uid").and_then(Value::as_u64).and_then(|value| i64::try_from(value).ok())).unwrap_or(0);
        let label = item.get("messageLabel").and_then(Value::as_str).unwrap_or_default();
        if uid == 0 || label.is_empty() { continue; }
        let layer = if game == ZeldaGame::TearsOfTheKingdom { layer_from_wire(item.get("layer").and_then(Value::as_str).unwrap_or_default()) } else { ZeldaLayer::Ground };
        if game == ZeldaGame::TearsOfTheKingdom && layer == ZeldaLayer::Unknown { continue; }
        output.push(MapPlace { uid, subcategory: subcategory.to_owned(), message_label: label.to_owned(), layer, position });
    }
    (!output.is_empty()).then_some(output)
}

fn wanted_subcategory(game: ZeldaGame, value: &str) -> bool {
    match game {
        ZeldaGame::TearsOfTheKingdom => matches!(value, "village" | "stable" | "structure" | "skyviewTower" | "other" | "shrine" | "lightroot"),
        ZeldaGame::BreathOfTheWild => matches!(value, "village" | "hatago" | "structure" | "tower" | "other" | "dungeon"),
        ZeldaGame::Unknown => false,
    }
}

fn fetch_labels(http: &HttpClient, session: &str, language: &str, country: &str) -> anyhow::Result<HashMap<String, String>> {
    let fetch = |locale: &str| -> anyhow::Result<HashMap<String, String>> {
        let response = http.get(&format!("{BASE_URL}/common/locales/{locale}/complete_guide.json"), &authenticated_headers(session, locale, country, "application/json,*/*"), 10, 2 * 1024 * 1024)?;
        if response.status() / 100 != 2 { return Ok(HashMap::new()); }
        let root: Value = serde_json::from_slice(response.body())?;
        Ok(root.as_object().map(|object| object.iter().filter_map(|(key, value)| value.as_str().map(|text| (key.clone(), text.trim().to_owned()))).collect()).unwrap_or_default())
    };
    let labels = fetch(language)?;
    if labels.is_empty() && language != "en-GB" { fetch("en-GB") } else { Ok(labels) }
}

fn html_unescape(value: &str) -> String { value.replace("&amp;", "&") }

fn percent_decode(value: &str) -> String {
    percent_encoding::percent_decode_str(value).decode_utf8_lossy().into_owned()
}

fn find_ugc_avatar(source: &str) -> String {
    let Some(marker) = source.find("storage.googleapis.com") else { return String::new(); };
    if let Some(start) = source[..marker].rfind("url=") {
        let value_start = start + 4;
        let tail = &source[value_start..];
        let end = tail.find(['&', '"', '\'', ' ']).unwrap_or(tail.len());
        return percent_decode(&html_unescape(&tail[..end]));
    }
    let Some(start) = source[..=marker].rfind("https://storage.googleapis.com") else { return String::new(); };
    let tail = &source[start..];
    let end = tail.find(['"', '\'', ' ']).unwrap_or(tail.len());
    html_unescape(&tail[..end])
}

fn percent_encode(value: &str) -> String {
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

fn router_state_tree(game: ZeldaGame) -> String {
    let raw = format!("[\"\",{{\"children\":[\"{}\",{{\"children\":[\"complete-guide\",{{\"children\":[\"__PAGE__\",{{}},null,null]}},null,null]}},null,null,true]}},null,null,true]", short_name(game));
    percent_encode(&raw)
}

fn resolve_location(metadata: &WebMetadata, state: &LiveState, previous: &ResolvedLocation) -> ResolvedLocation {
    let mut result = ResolvedLocation { layer: state.layer, region: resolve_region(metadata, state.game, state.position), ..ResolvedLocation::default() };
    let exact: LocationResult = match state.game { ZeldaGame::TearsOfTheKingdom => resolve_totk_location_3d(state.position, state.layer), ZeldaGame::BreathOfTheWild => resolve_botw_location_3d(state.position), ZeldaGame::Unknown => LocationResult::default() };
    if exact.matched() {
        result.poi = exact.name().to_owned(); result.stage_image_uri = exact.image_url().to_owned(); result.at_poi = true; result.valid = true; return result;
    }
    let mut best: Option<(&MapPlace, f64, f64)> = None;
    for place in &metadata.places {
        if state.game == ZeldaGame::TearsOfTheKingdom && place.layer != state.layer { continue; }
        let name = localized_label(metadata, &place.message_label);
        if name.is_empty() || name == "???" { continue; }
        let (_, nearby) = thresholds(&place.subcategory);
        let distance = horizontal_distance(state.position, place.position);
        if distance > nearby { continue; }
        let score = distance / nearby + f64::from(100 - category_priority(&place.subcategory)) * 0.004;
        if best.is_none_or(|(_, _, best_score)| score < best_score) { best = Some((place, distance, score)); }
    }
    if let Some((place, distance, _)) = best {
        let chosen = if previous.poi_uid != 0 && previous.poi_uid != place.uid {
            metadata.places.iter().find(|candidate| candidate.uid == previous.poi_uid && (state.game != ZeldaGame::TearsOfTheKingdom || candidate.layer == state.layer)).and_then(|old| {
                let (_, nearby) = thresholds(&old.subcategory); let old_distance = horizontal_distance(state.position, old.position);
                (old_distance <= nearby * 1.15 && distance > old_distance * 0.75).then_some((old, old_distance))
            }).unwrap_or((place, distance))
        } else { (place, distance) };
        let (place, distance) = chosen;
        result.poi = localized_label(metadata, &place.message_label); result.poi_uid = place.uid; result.poi_distance = distance; result.subcategory = place.subcategory.clone();
        result.stage_image_uri = resolve_poi_artwork(&result.poi, state.game);
        let (at, _) = thresholds(&place.subcategory); result.at_poi = distance <= at; result.near_poi = !result.at_poi;
    }
    result.valid = !result.region.is_empty() || !result.poi.is_empty();
    result
}

fn format_presence(_metadata: &WebMetadata, state: &LiveState, location: &ResolvedLocation) -> ZeldaNotesPresence {
    if !location.valid { return ZeldaNotesPresence::default(); }
    let details = if !location.poi.is_empty() && location.at_poi {
        format!("At {}", location.poi)
    } else if !location.poi.is_empty() {
        format!("Near {}", location.poi)
    } else if !location.region.is_empty() {
        format!("Exploring {}", location.region)
    } else {
        "Exploring Hyrule".to_owned()
    };
    let secondary = lore_activity(state, location);
    if details.is_empty() || secondary.is_empty() { return ZeldaNotesPresence::default(); }
    ZeldaNotesPresence {
        profile_summary: clamp_text(details),
        title_name: clamp_text(secondary),
        stage_image_uri: if location.stage_image_uri.is_empty() { resolve_region_artwork(&location.region, state.game, state.layer) } else { location.stage_image_uri.clone() },
        stage_name: if location.poi.is_empty() { if location.region.is_empty() { "Hyrule".to_owned() } else { location.region.clone() } } else { location.poi.clone() },
        avatar_url: String::new(),
        active: true,
    }
}

fn lore_activity(state: &LiveState, location: &ResolvedLocation) -> String {
    let poi = &location.poi;
    let region = &location.region;
    let category = &location.subcategory;
    let x = state.position.x();
    let y = state.position.y();
    let z = state.position.z();

    if location.at_poi || location.near_poi {
        if poi.contains("Hyrule Castle") || poi.contains("Sanctum") { return "Infiltrating the Sacred Seat of Hyrule".to_owned(); }
        if poi.contains("Temple of Time") { return "Sacred Ruins of the Ancient Realm".to_owned(); }
        if poi.contains("Forgotten Temple") { return "Exploring the Ancient Hidden Temple".to_owned(); }
        if poi.contains("Yiga") || poi.contains("Hideout") { return "Infiltrating Enemy Stronghold".to_owned(); }
        if poi.contains("Coliseum") || poi.contains("Colosseum") { return "Challenging Ancient Arenas".to_owned(); }
        if poi.contains("Citadel") { return "Exploring Ancient Citadel Ruins".to_owned(); }
        if poi.contains("Labyrinth") || poi.contains("Maze") { return "Navigating Ancient Labyrinths".to_owned(); }
        if poi.contains("Chasm") { return "Descending into the Depths".to_owned(); }
        if poi.contains("Cave") || poi.contains("Well") || poi.contains("Grotto") { return "Exploring Caverns & Tunnels".to_owned(); }
        if poi.contains("Fairy") { return "Visiting the Great Fairy".to_owned(); }
        if poi.contains("Lab") { return "Visiting the Ancient Tech Lab".to_owned(); }
        if poi.contains("Hudson Construction") { return "Building the Future of Hyrule".to_owned(); }
        if poi.contains("Tower") || matches!(category.as_str(), "skyviewTower" | "tower") {
            if matches!(region.as_str(), "Hebra" | "Tabantha") { return "Surveying the Frozen Mountain Peaks".to_owned(); }
            if region == "Akkala" { return "Surveying the Autumn Highlands".to_owned(); }
            if matches!(region.as_str(), "Eldin" | "Death Mountain") { return "Overlooking the Volcanic Realm".to_owned(); }
            if region == "Gerudo" { return "Overlooking the Shifting Sands".to_owned(); }
            if region == "Lanayru" { return "Surveying the Rushing Waterways".to_owned(); }
            if matches!(region.as_str(), "Necluda" | "Dueling Peaks") { return "Surveying the Peaceful Valleys".to_owned(); }
            if matches!(region.as_str(), "Faron" | "Lake Hylia") { return "Overlooking the Dense Jungle Wilds".to_owned(); }
            if region == "Great Plateau" { return "Surveying the Ancient Plateau".to_owned(); }
            return "Surveying from High Vantage Point".to_owned();
        }
        if poi.contains("Stable") || matches!(category.as_str(), "stable" | "hatago") {
            if matches!(region.as_str(), "Hebra" | "Tabantha") { return "Resting by the Frozen Snowfields".to_owned(); }
            if region == "Akkala" { return "Resting on the Road to the Sea".to_owned(); }
            if region == "Eldin" { return "Resting by the Foot of the Volcano".to_owned(); }
            if region == "Gerudo" { return "Resting on the Way to the Desert".to_owned(); }
            if region == "Lanayru" { return "Resting along the Waterways".to_owned(); }
            if matches!(region.as_str(), "Necluda" | "Dueling Peaks") { return "Resting near the Cleft of the Peaks".to_owned(); }
            if matches!(region.as_str(), "Faron" | "Lake Hylia") { return "Resting by the Southern Shores".to_owned(); }
            return "Resting at the Stable".to_owned();
        }
        if poi.contains("Shrine") || category == "shrine" { return "Investigating a Sacred Shrine".to_owned(); }
        if poi.contains("Lightroot") || category == "lightroot" { return "Resting in the Glow of a Lightroot".to_owned(); }
        if poi.contains("Temple") || poi.contains("Mine") || poi.contains("Forge") || category == "dungeon" { return "Delving into Ancient Ruins".to_owned(); }
        if (poi.contains("Archipelago") || poi.contains("Island")) && state.layer == ZeldaLayer::Sky { return "Navigating High Sky Islands".to_owned(); }
        if poi.contains("Tarrey Town") { return "Settlement on the Island Bluff".to_owned(); }
        if poi.contains("Lookout Landing") { return "Heart of the Resistance".to_owned(); }
        if poi.contains("Rito Village") { return "Home of the Champions".to_owned(); }
        if poi.contains("Goron City") { return "City in the Mountain Crags".to_owned(); }
        if poi.contains("Zora") && poi.contains("Domain") { return "Domain of the Water Tribe".to_owned(); }
        if poi.contains("Gerudo Town") || poi.contains("Kara Kara") { return "Oasis in the Desert Sands".to_owned(); }
        if poi.contains("Hateno Village") { return "Idyllic Countryside Pastures".to_owned(); }
        if poi.contains("Kakariko Village") { return "Hidden Haven of the Sheikah".to_owned(); }
        if poi.contains("Lurelin Village") { return "Tropical Seaside Haven".to_owned(); }
        if poi.contains("Korok Forest") { return "Sanctuary of the Great Deku Tree".to_owned(); }
        if category == "village" || poi.contains("Town") || poi.contains("Village") || poi.contains("Landing") || poi.contains("Domain") || poi.contains("City") { return "Visiting Local Settlement".to_owned(); }
    }

    if state.game == ZeldaGame::TearsOfTheKingdom && state.layer == ZeldaLayer::Sky {
        if y > 2200.0 { return "Soaring in the Upper Stratosphere".to_owned(); }
        if x > 3000.0 && z < -1000.0 { return "Navigating High Sky Islands".to_owned(); }
        if x < -2500.0 && z < -1500.0 { return "Navigating the Cold Sky Realm".to_owned(); }
        if x < -2500.0 && z > 1500.0 { return "Navigating the Desert Sky Realm".to_owned(); }
        if x > 1500.0 && z < -1500.0 { return "Navigating the Volcanic Sky Realm".to_owned(); }
        if x > 1500.0 && z > 1500.0 { return "Navigating the Southern Sky Realm".to_owned(); }
        if x > -500.0 && x < 1000.0 && z > 500.0 && z < 2000.0 { return "Exploring Ancient Sky Ruins".to_owned(); }
        return "Soaring above the Clouds".to_owned();
    }

    if state.game == ZeldaGame::TearsOfTheKingdom && state.layer == ZeldaLayer::Underground {
        if y < -800.0 { return "Trekking the Abyssal Depths".to_owned(); }
        if x > 2500.0 && z < -1000.0 { return "Trekking the Eastern Depths".to_owned(); }
        if x > 1000.0 && z < -2000.0 { return "Navigating the Volcanic Depths".to_owned(); }
        if x < -2000.0 && z > 1000.0 { return "Trekking the Desert Depths".to_owned(); }
        if x < -2000.0 && z < -1000.0 { return "Braving the Freezing Depths".to_owned(); }
        if x > 1500.0 && z > -500.0 && z < 1000.0 { return "Navigating the Underground Waterways".to_owned(); }
        if x > -1500.0 && x < 1500.0 && z > -1500.0 && z < 1500.0 { return "Trekking the Central Depths".to_owned(); }
        return "Trekking the Lightless Depths".to_owned();
    }

    if region == "Akkala" {
        if x > 4000.0 && z < -2000.0 { return "Wandering the Rist Peninsula Coast".to_owned(); }
        if z < -2800.0 { return "Traversing the Deep Highlands".to_owned(); }
        if z < -2200.0 && x < 3600.0 { return "Investigating Skull Lake".to_owned(); }
        if x > 3200.0 && z > -2000.0 && z < -1200.0 { return "Wandering around the Lake Caldera".to_owned(); }
        if z > -1200.0 { return "Traversing the Southern Plains".to_owned(); }
        return "Wandering the Autumn Highlands".to_owned();
    }
    if matches!(region.as_str(), "Central Hyrule" | "Hyrule Field") {
        if z < -1000.0 && x > -500.0 && x < 500.0 { return "Surveying Ancient Castle Town Ruins".to_owned(); }
        if x < -1000.0 { return "Roaming the Western Plains".to_owned(); }
        if x > 1000.0 { return "Wandering near Crenel Hills".to_owned(); }
        if z > 500.0 { return "Traversing the Vast Plains".to_owned(); }
        return "Roaming the Heart of the Plains".to_owned();
    }
    if matches!(region.as_str(), "Eldin" | "Death Mountain") {
        if x > 2000.0 && z < -2500.0 { return "Scaling the Summit of the Volcano".to_owned(); }
        if z < -3000.0 { return "Climbing the Northern Peaks".to_owned(); }
        if x < 1500.0 { return "Braving the Crags of the Canyon".to_owned(); }
        return "Traversing the Scorching Lava Beds".to_owned();
    }
    if matches!(region.as_str(), "Hebra" | "Tabantha") {
        if x < -2500.0 && z < -2500.0 { return "Braving the Summit of the Mountain".to_owned(); }
        if x > -2500.0 && z < -2500.0 { return "Traversing the Tundra Snowfields".to_owned(); }
        if z > -2000.0 { return "Wandering the Western Frontier".to_owned(); }
        return "Braving the Freezing Mountain Peaks".to_owned();
    }
    if region == "Gerudo" {
        if z > 2500.0 && x < -2500.0 { return "Traversing the Great Desert Dunes".to_owned(); }
        if z < 1500.0 { return "Scaling the Frozen Highlands".to_owned(); }
        if x > -2500.0 { return "Navigating the Narrow Canyons".to_owned(); }
        return "Traversing the Shifting Sands".to_owned();
    }
    if region == "Lanayru" {
        if x > 3000.0 && z > 500.0 { return "Braving the High Peak Snowfields".to_owned(); }
        if x < 2000.0 && z > -500.0 { return "Navigating the Vast Wetlands".to_owned(); }
        if x > 2500.0 && z < -500.0 { return "Roaming the Domain Waterways".to_owned(); }
        return "Roaming the Rushing Waterways".to_owned();
    }
    if matches!(region.as_str(), "Necluda" | "Dueling Peaks") {
        if x < 2000.0 { return "Traversing the Cleft of the Peaks".to_owned(); }
        if x > 3000.0 { return "Roaming the Eastern Valleys".to_owned(); }
        return "Wandering the Peaceful Countryside".to_owned();
    }
    if matches!(region.as_str(), "Faron" | "Lake Hylia") {
        if x < 500.0 && z > 2000.0 { return "Roaming the Shores of the Great Lake".to_owned(); }
        if x > 2500.0 { return "Wandering the Sunny Palmorae Coast".to_owned(); }
        return "Venturing through the Dense Jungle".to_owned();
    }
    if region == "Great Hyrule Forest" { return "Navigating the Mystical Lost Woods".to_owned(); }
    if region == "Hyrule Ridge" { return "Traversing the Windy Plateaus".to_owned(); }
    if region == "Great Plateau" { return "Trekking the Ancient Plateau".to_owned(); }
    "Roaming the Open Wilds".to_owned()
}

fn clamp_text(mut value: String) -> String {
    const MAX: usize = 128;
    if value.len() <= MAX { return value; }
    let mut end = MAX - 3;
    while !value.is_char_boundary(end) { end -= 1; }
    value.truncate(end); value.push_str("..."); value
}

struct TowerRegion { localized: &'static str, english: &'static str, x: f64, z: f64 }
const TOTK_TOWERS: &[TowerRegion] = &[
    TowerRegion { localized: "Ex_MapRegion_HyrulePrairie", english: "Central Hyrule", x: -298.85, z: -142.85 }, TowerRegion { localized: "Ex_MapRegion_HyrulePrairie", english: "Central Hyrule", x: -1909.588, z: -1245.305 },
    TowerRegion { localized: "Ex_MapRegion_Hebura", english: "Hebra", x: -2311.495, z: -3062.495 }, TowerRegion { localized: "Ex_MapRegion_Eldin", english: "Eldin", x: 1641.805, z: -1190.82 },
    TowerRegion { localized: "Ex_MapRegion_Tamul", english: "Akkala", x: 3499.0, z: -2026.0 }, TowerRegion { localized: "Ex_MapRegion_Hateru", english: "Necluda", x: 1341.109, z: 1177.858 },
    TowerRegion { localized: "Ex_MapRegion_Lanayru", english: "Lanayru", x: 2866.062, z: -581.1915 }, TowerRegion { localized: "Ex_MapRegion_HyrulePrairie", english: "Central Hyrule", x: -761.2766, z: 1019.228 },
    TowerRegion { localized: "Ex_MapRegion_Gerudo", english: "Gerudo", x: -2438.851, z: 2182.764 }, TowerRegion { localized: "Ex_MapRegion_Gerudo", english: "Gerudo", x: -3960.877, z: 1305.596 },
    TowerRegion { localized: "Ex_MapRegion_Hateru", english: "Necluda", x: 2420.0, z: 2754.891 }, TowerRegion { localized: "Ex_MapRegion_HyrulePrairie", english: "Central Hyrule", x: 343.6745, z: -3141.648 },
    TowerRegion { localized: "Ex_MapRegion_Firone", english: "Faron", x: 604.8388, z: 2126.876 }, TowerRegion { localized: "Ex_MapRegion_Lanayru", english: "Lanayru", x: 3847.638, z: 1314.911 },
    TowerRegion { localized: "Ex_MapRegion_Hebura", english: "Hebra", x: -3679.585, z: -2346.404 },
];
const BOTW_TOWERS: &[TowerRegion] = &[
    TowerRegion { localized: "U_MapRegion_Hebura", english: "Hebra", x: -2173.0, z: -2034.0 }, TowerRegion { localized: "U_MapRegion_Hebura", english: "Hebra", x: -3613.748, z: -990.1647 },
    TowerRegion { localized: "U_MapRegion_Gerudo", english: "Gerudo", x: -3666.0, z: 1828.6 }, TowerRegion { localized: "U_MapRegion_Gerudo", english: "Gerudo", x: -2306.836, z: 2437.32 },
    TowerRegion { localized: "U_MapRegion_HyrulePrairie ", english: "Central Hyrule", x: 883.8843, z: -1605.71 }, TowerRegion { localized: "U_MapRegion_HyrulePrairie ", english: "Central Hyrule", x: -788.645, z: 442.0306 },
    TowerRegion { localized: "U_MapRegion_HyrulePrairie ", english: "Central Hyrule", x: -560.0352, z: 1694.863 }, TowerRegion { localized: "U_MapRegion_Hateru", english: "Necluda", x: 1016.777, z: 1714.082 },
    TowerRegion { localized: "U_MapRegion_Firone", english: "Faron", x: -31.81555, z: 2961.601 }, TowerRegion { localized: "U_MapRegion_Eldin", english: "Eldin", x: 2174.151, z: -1556.781 },
    TowerRegion { localized: "U_MapRegion_Tamul", english: "Akkala", x: 3308.0, z: -1500.1 }, TowerRegion { localized: "U_MapRegion_Lanayru", english: "Lanayru", x: 2258.0, z: -109.0 },
    TowerRegion { localized: "U_MapRegion_Hateru", english: "Necluda", x: 2735.5, z: 2133.5 }, TowerRegion { localized: "U_MapRegion_Firone", english: "Faron", x: 1331.203, z: 3273.723 },
    TowerRegion { localized: "U_MapRegion_HyrulePrairie ", english: "Central Hyrule", x: -1755.3, z: -774.3 },
];

fn resolve_region(metadata: &WebMetadata, game: ZeldaGame, position: Vector3) -> String {
    let towers = if game == ZeldaGame::TearsOfTheKingdom { TOTK_TOWERS } else { BOTW_TOWERS };
    let nearest = towers.iter().min_by(|left, right| {
        let ld = (position.x() - left.x).powi(2) + (position.z() - left.z).powi(2);
        let rd = (position.x() - right.x).powi(2) + (position.z() - right.z).powi(2);
        ld.total_cmp(&rd)
    });
    nearest.map(|tower| { let localized = localized_label(metadata, tower.localized); if localized.is_empty() { tower.english.to_owned() } else { localized } }).unwrap_or_default()
}

fn localized_label(metadata: &WebMetadata, key: &str) -> String { metadata.labels.get(key).map(|value| value.trim().to_owned()).unwrap_or_default() }
fn horizontal_distance(left: Vector3, right: Vector3) -> f64 { ((left.x() - right.x()).powi(2) + (left.z() - right.z()).powi(2)).sqrt() }
fn thresholds(category: &str) -> (f64, f64) { match category { "village" => (180.0, 650.0), "stable" | "hatago" => (120.0, 450.0), "structure" => (110.0, 500.0), "other" => (100.0, 480.0), "skyviewTower" | "tower" => (90.0, 350.0), "shrine" | "dungeon" | "lightroot" => (65.0, 260.0), _ => (70.0, 250.0) } }
fn category_priority(category: &str) -> i32 { match category { "village" => 100, "stable" | "hatago" => 95, "structure" => 90, "other" => 85, "skyviewTower" | "tower" => 75, "shrine" | "dungeon" | "lightroot" => 60, _ => 40 } }
fn game_id(game: ZeldaGame) -> &'static str { match game { ZeldaGame::BreathOfTheWild => "0", ZeldaGame::TearsOfTheKingdom => "1", ZeldaGame::Unknown => "" } }
fn short_name(game: ZeldaGame) -> &'static str { match game { ZeldaGame::BreathOfTheWild => "botw", ZeldaGame::TearsOfTheKingdom => "totk", ZeldaGame::Unknown => "" } }
fn route_url(game: ZeldaGame) -> String {
    let short = short_name(game);
    if short.is_empty() { String::new() } else { format!("{BASE_URL}/{short}/complete-guide") }
}

fn bootstrap_headers(token: &str, language: &str, country: &str) -> Vec<String> {
    vec!["Upgrade-Insecure-Requests: 1".to_owned(), format!("User-Agent: {USER_AGENT}"), "x-appplatform: android".to_owned(), "x-appcolorscheme: DARK".to_owned(), format!("x-gamewebtoken: {token}"), "dnt: 1".to_owned(), "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8".to_owned(), format!("Accept-Language: {language}"), format!("X-NACountry: {country}"), "X-Requested-With: com.nintendo.znca".to_owned()]
}
fn authenticated_headers(session: &str, language: &str, country: &str, accept: &str) -> Vec<String> {
    vec![format!("User-Agent: {USER_AGENT}"), format!("Accept: {accept}"), format!("Accept-Language: {language}"), format!("X-NACountry: {country}"), format!("Cookie: {session}; lang={language}"), "dnt: 1".to_owned()]
}
fn session_cookie(response: &crate::http::HttpResponse) -> String {
    response.header("set-cookie").unwrap_or_default().lines().find_map(|line| {
        let trimmed = line.trim_start();
        let (name, rest) = trimmed.split_once('=')?;
        let lower = name.to_ascii_lowercase();
        if lower != "a5_token" && !lower.contains("session") { return None; }
        let value = rest.split(';').next().unwrap_or(rest);
        Some(format!("{name}={value}"))
    }).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::{LiveMessageType, decode_live_message, game_for_presence};
    use crate::zelda_regions::{ZeldaGame, ZeldaLayer};
    use std::time::Instant;

    #[test]
    fn decodes_totk_player_state() {
        let message = decode_live_message(
            r#"{"messageType":"map_sync_player_info","playerPos":[1,2,3],"playerFront":[0,0,1],"playerLayer":"Ground"}"#,
            ZeldaGame::TearsOfTheKingdom,
            Instant::now(),
        );
        assert_eq!(message.message_type, LiveMessageType::MapSyncPlayerInfo);
        let state = message.state.expect("state");
        assert_eq!(state.layer, ZeldaLayer::Ground);
        assert!(state.synchronized);
    }

    #[test]
    fn malformed_player_update_still_invalidates_live_state() {
        let message = decode_live_message(
            r#"{"messageType":"map_sync_player_info","playerPos":[1,2,3],"playerLayer":"Ground"}"#,
            ZeldaGame::TearsOfTheKingdom,
            Instant::now(),
        );
        assert!(message.valid);
        assert!(message.updates_live_state);
        assert!(!message.state.expect("state").synchronized);
    }

    #[test]
    fn recognizes_zelda_titles() { assert_eq!(game_for_presence("0100f2c0115b6000", ""), ZeldaGame::TearsOfTheKingdom); }
}
