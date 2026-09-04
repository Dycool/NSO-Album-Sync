//! Discord IPC Rich Presence implementation with Zelda live overlay support.

use crate::model::NintendoPresence;
use crate::util::{is_https_image_url, unix_seconds_from_millis_or_seconds};
use crate::zelda_notes::{ZeldaNotesPresence, game_for_presence};
use crate::zelda_regions::ZeldaGame;
use discord_rich_presence::{DiscordIpc, DiscordIpcClient, activity};
use std::sync::Mutex;

const MAX_IMAGE_URL: usize = 300;
const NOOKLINK_ORIGIN: &str = "https://web.sd.lp1.acbaa.srv.nintendo.net";
const SPLATNET2_ORIGIN: &str = "https://app.splatoon2.nintendo.net";

pub struct DiscordPresence {
    application_id: u64,
    client: Mutex<Option<DiscordIpcClient>>,
    last_base: Mutex<Option<NintendoPresence>>,
}

impl DiscordPresence {
    pub fn new(application_id: u64) -> Self { Self { application_id, client: Mutex::new(None), last_base: Mutex::new(None) } }

    pub fn self_test_runtime(&self) -> bool { self.ensure_connected().is_ok() }

    pub fn clear(&self) {
        *self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
        let mut guard = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(client) = guard.as_mut() { let _ = client.clear_activity(); }
    }

    pub fn update(&self, presence: &NintendoPresence, zelda: &ZeldaNotesPresence) {
        if !presence.is_playing() { self.clear(); return; }
        *self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(presence.clone());
        let effective = with_zelda_overlay(presence, zelda);
        let _ = self.publish(&effective);
    }

    pub fn refresh_zelda(&self, zelda: &ZeldaNotesPresence) {
        let base = self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).clone();
        if let Some(base) = base.filter(NintendoPresence::is_playing) { let _ = self.publish(&with_zelda_overlay(&base, zelda)); }
    }

    fn ensure_connected(&self) -> anyhow::Result<()> {
        let mut guard = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if guard.is_some() { return Ok(()); }
        let mut client = DiscordIpcClient::new(self.application_id.to_string());
        client.connect()?;
        *guard = Some(client);
        Ok(())
    }

    fn publish(&self, presence: &NintendoPresence) -> anyhow::Result<()> {
        self.ensure_connected()?;
        let mut activity = activity::Activity::new()
            .activity_type(activity::ActivityType::Playing)
            .status_display_type(activity::StatusDisplayType::Name)
            .name(presence.game_name().to_owned());
        let animal_crossing = is_animal_crossing(presence);
        let details = if !presence.custom_details().is_empty() { presence.custom_details().to_owned() } else { presence.discord_state() };
        if !details.is_empty() { activity = activity.details(details); }
        let state = if animal_crossing && !presence.custom_details().is_empty() {
            let playtime = presence.discord_state(); if playtime.is_empty() { presence.console_name().to_owned() } else { playtime }
        } else if !presence.custom_state().is_empty() { presence.custom_state().to_owned() } else { presence.console_name().to_owned() };
        activity = activity.state(state);
        if presence.updated_at() > 0 {
            let seconds = unix_seconds_from_millis_or_seconds(presence.updated_at());
            if let Some(milliseconds) = seconds.checked_mul(1000) { activity = activity.timestamps(activity::Timestamps::new().start(milliseconds)); }
        }

        let large = normalize_image_url(presence, if presence.custom_large_image_uri().is_empty() { presence.image_uri() } else { presence.custom_large_image_uri() });
        let small = normalize_image_url(presence, presence.custom_image_uri());
        if is_https_image_url(&large, MAX_IMAGE_URL) || is_https_image_url(&small, MAX_IMAGE_URL) {
            let mut assets = activity::Assets::new();
            if is_https_image_url(&large, MAX_IMAGE_URL) {
                let text = if presence.custom_large_text().is_empty() { presence.game_name() } else { presence.custom_large_text() };
                assets = assets.large_image(large).large_text(text.to_owned());
                if is_https_image_url(presence.shop_uri(), MAX_IMAGE_URL) { assets = assets.large_url(presence.shop_uri().to_owned()); }
            }
            if is_https_image_url(&small, MAX_IMAGE_URL) {
                let text = if !presence.user_name().is_empty() { presence.user_name() } else { presence.custom_details() };
                assets = assets.small_image(small);
                if !text.is_empty() { assets = assets.small_text(text.to_owned()); }
            }
            activity = activity.assets(assets);
        }

        let mut guard = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let Some(client) = guard.as_mut() else { return Ok(()); };
        if client.set_activity(activity.clone()).is_err() {
            let _ = client.reconnect();
            client.set_activity(activity)?;
        }
        Ok(())
    }
}

impl Drop for DiscordPresence {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.client.lock()
            && let Some(client) = guard.as_mut()
        {
            let _ = client.clear_activity();
            let _ = client.close();
        }
    }
}

fn with_zelda_overlay(base: &NintendoPresence, zelda: &ZeldaNotesPresence) -> NintendoPresence {
    let mut effective = base.clone();
    if game_for_presence(base.title_id(), base.game_name()) == ZeldaGame::Unknown || !zelda.active() { return effective; }
    if !zelda.format_details().is_empty() { effective.set_custom_details(zelda.format_details().to_owned()); }
    if !zelda.format_state().is_empty() { effective.set_custom_state(zelda.format_state().to_owned()); }
    if !zelda.stage_image_uri().is_empty() { effective.set_custom_large_image(zelda.stage_image_uri().to_owned(), if zelda.stage_name().is_empty() { base.game_name().to_owned() } else { zelda.stage_name().to_owned() }); }
    effective
}

fn normalize_image_url(presence: &NintendoPresence, value: &str) -> String {
    if value.is_empty() { return String::new(); }
    let origin = if is_animal_crossing(presence) { Some(NOOKLINK_ORIGIN) } else if is_splatoon2(presence) { Some(SPLATNET2_ORIGIN) } else { None };
    let mut url = value.to_owned();
    if let Some(origin) = origin {
        if url.starts_with("//") { url = format!("https:{url}"); }
        else if url.starts_with('/') { url = format!("{origin}{url}"); }
        else if !url.contains("://") && !url.starts_with("data:") { url = format!("{origin}/{url}"); }
    }
    url
}

fn is_animal_crossing(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01006f8002326000" || presence.game_name().contains("Animal Crossing") || presence.game_name().contains("あつまれ どうぶつの森")
}

fn is_splatoon2(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01003bc0000a0000" || presence.game_name().contains("Splatoon 2") || presence.game_name().contains("スプラトゥーン2")
}