//! Discord Social SDK Rich Presence implementation with Zelda live overlay support.

use crate::model::NintendoPresence;
use crate::zelda_notes::{ZeldaNotesPresence, game_for_presence};
use crate::zelda_regions::ZeldaGame;
use nso_discord_social_sdk::{Activity, Client};
use std::sync::Mutex;

const MAX_IMAGE_URL: usize = 300;
const NOOKLINK_ORIGIN: &str = "https://web.sd.lp1.acbaa.srv.nintendo.net";
const SPLATNET2_ORIGIN: &str = "https://app.splatoon2.nintendo.net";

pub struct DiscordPresence {
    application_id: u64,
    client: Mutex<Option<Client>>,
    last_base: Mutex<Option<NintendoPresence>>,
}

impl DiscordPresence {
    pub fn new(application_id: u64) -> Self {
        Self {
            application_id,
            client: Mutex::new(None),
            last_base: Mutex::new(None),
        }
    }

    pub fn self_test_runtime(&self) -> bool {
        let mut guard = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if guard.is_none() {
            *guard = Client::new(self.application_id);
        }
        guard.as_ref().is_some_and(Client::self_test_runtime)
    }

    pub fn clear(&self) {
        *self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
        if let Some(client) = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).as_ref() {
            client.clear();
        }
    }

    pub fn update(&self, presence: &NintendoPresence, zelda: &ZeldaNotesPresence) {
        if !presence.is_playing() {
            self.clear();
            return;
        }
        *self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(presence.clone());
        self.publish(&with_zelda_overlay(presence, zelda));
    }

    pub fn refresh_zelda(&self, zelda: &ZeldaNotesPresence) {
        let base = self.last_base.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).clone();
        if let Some(base) = base.filter(NintendoPresence::is_playing) {
            self.publish(&with_zelda_overlay(&base, zelda));
        }
    }

    fn publish(&self, presence: &NintendoPresence) {
        let mut guard = self.client.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if guard.is_none() {
            *guard = Client::new(self.application_id);
        }
        let Some(client) = guard.as_ref() else { return; };

        let animal_crossing = is_animal_crossing(presence);
        let details = if !presence.custom_details().is_empty() {
            presence.custom_details().to_owned()
        } else {
            presence.discord_state()
        };
        let state = if animal_crossing && !presence.custom_details().is_empty() {
            let playtime = presence.discord_state();
            if playtime.is_empty() {
                presence.console_name().to_owned()
            } else {
                playtime
            }
        } else if !presence.custom_state().is_empty() {
            presence.custom_state().to_owned()
        } else {
            presence.console_name().to_owned()
        };

        let start_seconds = if presence.updated_at() > 0 {
            let value = if presence.updated_at() > 10_000_000_000 {
                presence.updated_at() / 1000
            } else {
                presence.updated_at()
            };
            u64::try_from(value).unwrap_or(0)
        } else {
            0
        };

        let large = normalize_image_url(
            presence,
            if presence.custom_large_image_uri().is_empty() {
                presence.image_uri()
            } else {
                presence.custom_large_image_uri()
            },
        );
        let small = normalize_image_url(presence, presence.custom_image_uri());
        if !presence.custom_image_uri().is_empty() && !is_valid_image_url(&small) {
            log_rejected_custom_image(presence, &small);
        }

        let has_large = is_valid_image_url(&large);
        let has_small = is_valid_image_url(&small);
        let large_image = if has_large { large.as_str() } else { "" };
        let large_text = if has_large {
            if presence.custom_large_text().is_empty() {
                presence.game_name()
            } else {
                presence.custom_large_text()
            }
        } else {
            ""
        };
        let large_url = if has_large && is_valid_image_url(presence.shop_uri()) {
            presence.shop_uri()
        } else {
            ""
        };
        let small_image = if has_small { small.as_str() } else { "" };
        let small_text = if has_small {
            if !presence.user_name().is_empty() {
                presence.user_name()
            } else if !presence.custom_details().is_empty() {
                presence.custom_details()
            } else {
                ""
            }
        } else {
            ""
        };

        let _ = client.update(&Activity {
            name: presence.game_name(),
            details: &details,
            state: &state,
            start_seconds,
            large_image,
            large_text,
            large_url,
            small_image,
            small_text,
        });
    }
}

impl Drop for DiscordPresence {
    fn drop(&mut self) {
        self.clear();
    }
}

fn with_zelda_overlay(base: &NintendoPresence, zelda: &ZeldaNotesPresence) -> NintendoPresence {
    let mut effective = base.clone();
    if game_for_presence(base.title_id(), base.game_name()) == ZeldaGame::Unknown || !zelda.active() {
        return effective;
    }
    if !zelda.format_details().is_empty() {
        effective.set_custom_details(zelda.format_details().to_owned());
    }
    if !zelda.format_state().is_empty() {
        effective.set_custom_state(zelda.format_state().to_owned());
    }
    if !zelda.stage_image_uri().is_empty() {
        effective.set_custom_large_image(
            zelda.stage_image_uri().to_owned(),
            if zelda.stage_name().is_empty() {
                base.game_name().to_owned()
            } else {
                zelda.stage_name().to_owned()
            },
        );
    }
    effective
}

fn normalize_image_url(presence: &NintendoPresence, value: &str) -> String {
    if value.is_empty() {
        return String::new();
    }
    let origin = if is_animal_crossing(presence) {
        Some(NOOKLINK_ORIGIN)
    } else if is_splatoon2(presence) {
        Some(SPLATNET2_ORIGIN)
    } else {
        None
    };
    let mut url = value.to_owned();
    if let Some(origin) = origin {
        if url.starts_with("//") {
            url = format!("https:{url}");
        } else if url.starts_with('/') {
            url = format!("{origin}{url}");
        } else if !url.contains("://") && !url.starts_with("data:") {
            url = format!("{origin}/{url}");
        }
    }
    url
}

fn is_valid_image_url(url: &str) -> bool {
    !url.is_empty()
        && url.len() <= MAX_IMAGE_URL
        && url.starts_with("https://")
        && !url.bytes().any(|byte| byte.is_ascii_whitespace())
}

fn log_rejected_custom_image(presence: &NintendoPresence, url: &str) {
    if url.is_empty() {
        return;
    }
    let reason = if url.len() > MAX_IMAGE_URL {
        format!("URL too long ({} chars)", url.len())
    } else if !url.starts_with("https://") {
        "not an HTTPS URL".to_owned()
    } else if url.bytes().any(|byte| byte.is_ascii_whitespace()) {
        "contains whitespace".to_owned()
    } else {
        return;
    };
    eprintln!(
        "[DiscordPresence] Custom image rejected for {}: {reason}",
        presence.game_name()
    );
}

fn is_animal_crossing(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01006f8002326000"
        || presence.game_name().contains("Animal Crossing")
        || presence.game_name().contains("あつまれ どうぶつの森")
}

fn is_splatoon2(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01003bc0000a0000"
        || presence.game_name().contains("Splatoon 2")
        || presence.game_name().contains("スプラトゥーン2")
}
