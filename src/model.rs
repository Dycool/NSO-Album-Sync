//! Shared data models with private fields and invariant-preserving constructors.

use serde::{Deserialize, Serialize};

pub const DISCORD_APPLICATION_ID: u64 = 1_538_902_170_433_495_172;
pub const SPLATOON3_GAME_SERVICE_ID: u64 = 4_834_290_508_791_808;
pub const SPLATOON2_GAME_SERVICE_ID: u64 = 5_741_031_244_955_648;
pub const ANIMAL_CROSSING_GAME_SERVICE_ID: u64 = 4_953_919_198_265_344;
pub const ZELDA_NOTES_GAME_SERVICE_ID: u64 = 5_935_781_783_175_168;
pub const ZELDA_NOTES_GAME_SERVICE_ID_ALT: u64 = 4_974_384_874_151_936;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(default, rename_all = "camelCase")]
pub struct AppConfig {
    session_token: String,
    user_nickname: String,
    destination_folder: String,
    auto_sync: bool,
    auto_sync_setting_version: u32,
    notifications: bool,
    discord_presence: bool,
    discord_presence_setting_version: u32,
    start_on_boot: bool,
    sync_interval_minutes: u32,
    last_sync: String,
    proxy_url: String,
    nxapi_auth_client_id: String,
    discord_application_id: u64,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            session_token: String::new(),
            user_nickname: "Nintendo Switch Player".to_owned(),
            destination_folder: String::new(),
            auto_sync: false,
            auto_sync_setting_version: 1,
            notifications: false,
            discord_presence: false,
            discord_presence_setting_version: 1,
            start_on_boot: false,
            sync_interval_minutes: 60,
            last_sync: "Never".to_owned(),
            proxy_url: String::new(),
            nxapi_auth_client_id: "eJ8TDme0c-Z4czx5SvZabA".to_owned(),
            discord_application_id: DISCORD_APPLICATION_ID,
        }
    }
}

impl AppConfig {
    pub fn session_token(&self) -> &str { &self.session_token }
    pub fn user_nickname(&self) -> &str { &self.user_nickname }
    pub fn destination_folder(&self) -> &str { &self.destination_folder }
    pub fn auto_sync(&self) -> bool { self.auto_sync }
    pub fn notifications(&self) -> bool { self.notifications }
    pub fn discord_presence(&self) -> bool { self.discord_presence }
    pub fn start_on_boot(&self) -> bool { self.start_on_boot }
    pub fn sync_interval_minutes(&self) -> u32 { self.sync_interval_minutes.max(1) }
    pub fn last_sync(&self) -> &str { &self.last_sync }
    pub fn proxy_url(&self) -> &str { &self.proxy_url }
    pub fn nxapi_auth_client_id(&self) -> &str { &self.nxapi_auth_client_id }
    pub fn discord_application_id(&self) -> u64 { self.discord_application_id }

    pub fn set_session(&mut self, token: String, nickname: String) {
        self.session_token = token;
        self.user_nickname = if nickname.trim().is_empty() {
            "Nintendo Switch Player".to_owned()
        } else {
            nickname
        };
    }

    pub fn clear_session(&mut self) {
        self.session_token.clear();
        self.user_nickname = "Nintendo Switch Player".to_owned();
    }

    pub fn set_destination_folder(&mut self, path: String) { self.destination_folder = path; }
    pub fn toggle_auto_sync(&mut self) { self.auto_sync = !self.auto_sync; self.auto_sync_setting_version = 1; }
    pub fn toggle_notifications(&mut self) { self.notifications = !self.notifications; }
    pub fn toggle_discord_presence(&mut self) { self.discord_presence = !self.discord_presence; self.discord_presence_setting_version = 1; }
    pub fn set_start_on_boot(&mut self, enabled: bool) { self.start_on_boot = enabled; }
    pub fn set_sync_interval_minutes(&mut self, minutes: u32) { self.sync_interval_minutes = minutes.max(1); }
    pub fn set_last_sync(&mut self, value: String) { self.last_sync = value; }
    pub fn set_proxy_url(&mut self, value: String) { self.proxy_url = value.trim().to_owned(); }

    pub fn force_secure_defaults(&mut self) {
        self.auto_sync_setting_version = 1;
        self.discord_presence_setting_version = 1;
        self.discord_application_id = DISCORD_APPLICATION_ID;
        if self.sync_interval_minutes == 0 { self.sync_interval_minutes = 60; }
        if self.nxapi_auth_client_id.trim().is_empty() {
            self.nxapi_auth_client_id = "eJ8TDme0c-Z4czx5SvZabA".to_owned();
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaItem {
    #[serde(default)]
    id: String,
    #[serde(default, alias = "applicationId")]
    title_id: String,
    #[serde(default = "default_app_name")]
    app_name: String,
    #[serde(rename = "type", default = "default_media_type")]
    media_type: String,
    #[serde(default)]
    content_uri: String,
    #[serde(default)]
    thumbnail_uri: String,
    #[serde(default)]
    content_length: i64,
    #[serde(default)]
    captured_at: i64,
    #[serde(default)]
    uploaded_at: i64,
    #[serde(default)]
    expires_at: i64,
}

fn default_app_name() -> String { "Nintendo Switch".to_owned() }
fn default_media_type() -> String { "image".to_owned() }

impl MediaItem {
    pub fn id(&self) -> &str { &self.id }
    pub fn title_id(&self) -> &str { &self.title_id }
    pub fn app_name(&self) -> &str { &self.app_name }
    pub fn media_type(&self) -> &str { &self.media_type }
    pub fn content_uri(&self) -> &str { &self.content_uri }
    pub fn thumbnail_uri(&self) -> &str { &self.thumbnail_uri }
    pub fn content_length(&self) -> i64 { self.content_length }
    pub fn captured_at(&self) -> i64 { self.captured_at }
    pub fn uploaded_at(&self) -> i64 { self.uploaded_at }
    pub fn expires_at(&self) -> i64 { self.expires_at }
    pub fn timestamp(&self) -> i64 { if self.captured_at != 0 { self.captured_at } else { self.uploaded_at } }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct NintendoPresence {
    state: String,
    platform: String,
    user_name: String,
    game_name: String,
    title_id: String,
    image_uri: String,
    shop_uri: String,
    sys_description: String,
    updated_at: i64,
    total_play_time: i64,
    custom_state: String,
    custom_details: String,
    custom_image_uri: String,
    custom_large_image_uri: String,
    custom_large_text: String,
}

impl NintendoPresence {
    pub fn from_coral_result(result: &serde_json::Value) -> Self {
        let mut output = Self {
            user_name: string_at(result, "name").or_else(|| string_at(result, "nickname")).unwrap_or_default(),
            custom_image_uri: string_at(result, "imageUri").or_else(|| string_at(result, "image2Uri")).unwrap_or_default(),
            ..Self::default()
        };
        let Some(presence) = result.get("presence") else { return output; };
        output.state = string_at(presence, "state").unwrap_or_else(|| "OFFLINE".to_owned());
        output.updated_at = integer_at(presence, "updatedAt").unwrap_or_default();
        if let Some(platform) = presence.get("platform") {
            output.platform = value_as_string(platform).unwrap_or_default();
        }
        if let Some(game) = presence.get("game") {
            output.game_name = string_at(game, "name").unwrap_or_default();
            output.image_uri = string_at(game, "imageUri").unwrap_or_default();
            output.shop_uri = string_at(game, "shopUri").unwrap_or_default();
            output.sys_description = string_at(game, "sysDescription").unwrap_or_default();
            output.total_play_time = integer_at(game, "totalPlayTime").unwrap_or_default();
            output.title_id = string_at(game, "titleId").or_else(|| string_at(game, "applicationId")).unwrap_or_default();
            if output.title_id.is_empty()
                && let Some(id) = game.get("id")
                && let Some(text) = value_as_string(id)
            {
                if let Ok(number) = text.parse::<u64>() {
                    if number > 0 { output.title_id = format!("{number:016x}"); }
                } else {
                    output.title_id = text;
                }
            }
            if output.title_id.is_empty()
                && let Some(index) = output.shop_uri.find("/apps/")
            {
                let start = index + 6;
                if output.shop_uri.len() >= start + 16 {
                    let candidate = &output.shop_uri[start..start + 16];
                    if candidate.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                        output.title_id = candidate.to_ascii_lowercase();
                    }
                }
            }
            output.title_id.make_ascii_lowercase();
        }
        output
    }

    pub fn state(&self) -> &str { &self.state }
    pub fn platform(&self) -> &str { &self.platform }
    pub fn user_name(&self) -> &str { &self.user_name }
    pub fn game_name(&self) -> &str { &self.game_name }
    pub fn title_id(&self) -> &str { &self.title_id }
    pub fn image_uri(&self) -> &str { &self.image_uri }
    pub fn shop_uri(&self) -> &str { &self.shop_uri }
    pub fn updated_at(&self) -> i64 { self.updated_at }
    pub fn custom_state(&self) -> &str { &self.custom_state }
    pub fn custom_details(&self) -> &str { &self.custom_details }
    pub fn custom_image_uri(&self) -> &str { &self.custom_image_uri }
    pub fn custom_large_image_uri(&self) -> &str { &self.custom_large_image_uri }
    pub fn custom_large_text(&self) -> &str { &self.custom_large_text }
    pub fn set_custom_state(&mut self, value: String) { self.custom_state = value; }
    pub fn set_custom_details(&mut self, value: String) { self.custom_details = value; }
    pub fn set_custom_image_uri(&mut self, value: String) { self.custom_image_uri = value; }
    pub fn set_custom_large_image(&mut self, uri: String, text: String) { self.custom_large_image_uri = uri; self.custom_large_text = text; }

    pub fn is_playing(&self) -> bool {
        self.state.eq_ignore_ascii_case("ONLINE") || self.state.eq_ignore_ascii_case("PLAYING")
    }

    pub fn console_name(&self) -> &'static str {
        match self.platform.to_ascii_uppercase().as_str() {
            "2" | "OUNCE" | "SWITCH_2" | "SWITCH2" | "NINTENDO_SWITCH_2" => "Nintendo Switch 2",
            _ => "Nintendo Switch",
        }
    }

    pub fn discord_state(&self) -> String {
        if !self.sys_description.is_empty() { return self.sys_description.clone(); }
        if self.total_play_time <= 0 { return String::new(); }
        let played_hours = self.total_play_time / 60;
        if played_hours < 5 { return "Played for a little while".to_owned(); }
        format!("Played for {} hours or more", (played_hours / 5) * 5)
    }
}

fn value_as_string(value: &serde_json::Value) -> Option<String> {
    value.as_str().map(ToOwned::to_owned)
        .or_else(|| value.as_i64().map(|v| v.to_string()))
        .or_else(|| value.as_u64().map(|v| v.to_string()))
}

fn string_at(value: &serde_json::Value, key: &str) -> Option<String> { value.get(key).and_then(value_as_string) }
fn integer_at(value: &serde_json::Value, key: &str) -> Option<i64> { value.get(key).and_then(|v| v.as_i64().or_else(|| v.as_u64().and_then(|n| i64::try_from(n).ok()))) }

#[derive(Debug, Clone, Default)]
pub struct SyncResult {
    total_found: usize,
    new_downloads: usize,
}

impl SyncResult {
    pub fn new(total_found: usize, new_downloads: usize) -> Self { Self { total_found, new_downloads } }
    pub fn total_found(&self) -> usize { self.total_found }
    pub fn new_downloads(&self) -> usize { self.new_downloads }
}

#[derive(Debug, Clone, Default)]
pub struct MenuState {
    nickname: String,
    last_sync: String,
    status: String,
    auto_sync: bool,
    notifications: bool,
    discord: bool,
    start_on_boot: bool,
    signed_in: bool,
    sync_interval_minutes: u32,
}

impl MenuState {
    pub fn new(config: &AppConfig, status: String, start_on_boot: bool) -> Self {
        Self {
            nickname: config.user_nickname().to_owned(),
            last_sync: config.last_sync().to_owned(),
            status,
            auto_sync: config.auto_sync(),
            notifications: config.notifications(),
            discord: config.discord_presence(),
            start_on_boot,
            signed_in: !config.session_token().is_empty(),
            sync_interval_minutes: config.sync_interval_minutes(),
        }
    }

    pub fn nickname(&self) -> &str { &self.nickname }
    pub fn last_sync(&self) -> &str { &self.last_sync }
    pub fn status(&self) -> &str { &self.status }
    pub fn auto_sync(&self) -> bool { self.auto_sync }
    pub fn notifications(&self) -> bool { self.notifications }
    pub fn discord(&self) -> bool { self.discord }
    pub fn start_on_boot(&self) -> bool { self.start_on_boot }
    pub fn signed_in(&self) -> bool { self.signed_in }
    pub fn sync_interval_minutes(&self) -> u32 { self.sync_interval_minutes }
}

#[cfg(test)]
mod tests {
    use super::{MediaItem, NintendoPresence};
    use serde_json::json;

    #[test]
    fn parses_switch_2_presence() {
        let presence = NintendoPresence::from_coral_result(&json!({
            "presence": {
                "state": "ONLINE",
                "platform": "OUNCE",
                "game": {"name": "Test", "titleId": "0100000000000000", "totalPlayTime": 600}
            }
        }));
        assert!(presence.is_playing());
        assert_eq!(presence.console_name(), "Nintendo Switch 2");
        assert_eq!(presence.discord_state(), "Played for 10 hours or more");
    }

    #[test]
    fn media_uses_coral_type_and_legacy_application_id() {
        let media: MediaItem = serde_json::from_value(json!({
            "applicationId": "0100000000000000",
            "type": "video",
            "contentUri": "https://example.com/capture",
            "contentLength": 1024
        })).expect("media deserializes");
        assert_eq!(media.title_id(), "0100000000000000");
        assert_eq!(media.media_type(), "video");
    }
}
