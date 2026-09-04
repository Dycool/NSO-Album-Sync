//! Desktop application orchestration and tray event loop.

use crate::auth_callback::{callback_from_args, publish_callback, register_protocol, take_callback};
use crate::config::{ConfigManager, config_directory};
use crate::coral::CoralClient;
use crate::discord::DiscordPresence;
use crate::game_services::{ANIMAL_CROSSING_GAME_SERVICE_ID, GameServicesClient, SPLATOON2_GAME_SERVICE_ID};
use crate::http::HttpClient;
use crate::model::{AppConfig, NintendoPresence, SyncResult};
use crate::nintendo_auth::NintendoAuthManager;
use crate::nxapi::NxapiClient;
use crate::platform;
use crate::single_instance::SingleInstance;
use crate::splatnet::{SPLATNET3_GAME_SERVICE_ID, SPLATNET3_GAME_SERVICE_ID_ALT, SplatNetClient};
use crate::sync::SyncEngine;
use crate::zelda_notes::{
    ZELDA_NOTES_GAME_SERVICE_ID, ZELDA_NOTES_GAME_SERVICE_ID_ALT, ZeldaNotesClient,
    game_for_presence,
};
use crate::zelda_regions::ZeldaGame;
use rand::Rng as _;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};
use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoop};
use tray_icon::menu::{Menu, MenuEvent, MenuItem};
use tray_icon::{Icon, TrayIconBuilder};

const PRESENCE_POLL_INTERVAL: Duration = Duration::from_secs(60);
const NXAPI_DISCLOSURE_TITLE: &str = "Third-Party Service Disclosure";
const NXAPI_DISCLOSURE: &str = "NSO Album Sync uses the third-party nxapi-znca-api service at fancy.org.uk for Nintendo Switch Online request attestation and request/response encryption.\n\nWhen Nintendo Switch Online features are used, your Nintendo Account id_token and the profile fields required by Coral (Nintendo Account ID, birthday, country, and language), your Coral access token, and the Coral API requests and responses used by this app are sent to and processed by that third-party service. These tokens can authenticate Nintendo services while they remain valid.\n\nContinue with Nintendo Account sign-in?";

pub fn run(args: Vec<String>) -> anyhow::Result<()> {
    let callback = callback_from_args(&args);
    let instance = SingleInstance::acquire()?;
    if instance.is_none() {
        if let Some(callback) = callback {
            publish_callback(&callback)?;
        }
        return Ok(());
    }
    let _instance = instance.expect("single-instance guard was just checked");
    register_protocol()?;

    let config = ConfigManager::load()?;
    apply_cli_settings(&config, &args)?;
    let initial = config.snapshot();
    let http = HttpClient::new(initial.proxy_url().to_owned());
    let auth = Arc::new(NintendoAuthManager::new(http.clone()));
    let nxapi_cache = config_directory()?.join("nxapi-cache.json");
    let nxapi = Arc::new(NxapiClient::new(
        http.clone(),
        initial.nxapi_auth_client_id().to_owned(),
        nxapi_cache,
    ));
    let coral = Arc::new(CoralClient::new(http.clone(), Arc::clone(&auth), Arc::clone(&nxapi)));
    let splatnet = Arc::new(SplatNetClient::new(http.clone()));
    let game_services = Arc::new(GameServicesClient::new(http.clone()));
    let zelda = Arc::new(ZeldaNotesClient::new(http.clone()));
    let sync_engine = Arc::new(SyncEngine::new(config.clone(), Arc::clone(&coral), http));
    let discord = Arc::new(DiscordPresence::new(initial.discord_application_id()));
    let enrichment_state = Arc::new(Mutex::new(PresenceEnrichmentState::default()));

    let (sender, receiver) = mpsc::channel::<AppEvent>();
    let stop = Arc::new(AtomicBool::new(false));
    let mut sync_running = false;
    let mut auth_running = false;
    let mut auth_pending = false;
    let mut presence_running = false;
    let session_ready = !initial.session_token().is_empty();
    let mut initial_sync_deferred =
        initial.auto_sync() && initial.discord_presence() && session_ready;
    let mut next_auto_sync = if initial.auto_sync() && session_ready && !initial_sync_deferred {
        Instant::now()
    } else {
        jittered_deadline(sync_interval(&initial))
    };
    let mut next_presence = Instant::now();
    let mut last_zelda = zelda.live_presence();

    let event_loop = EventLoop::new();
    let menu = TrayMenu::new(&config.snapshot(), platform::start_on_boot_enabled())?;
    let tray_menu = menu.root.clone();
    let mut pending_icon = Some(app_icon()?);
    let mut tray_icon = None;

    event_loop.run(move |event, _, control_flow| {
        *control_flow = ControlFlow::WaitUntil(Instant::now() + Duration::from_millis(500));
        let Event::NewEvents(cause) = event else {
            return;
        };

        if matches!(cause, StartCause::Init) && tray_icon.is_none() {
            let Some(icon) = pending_icon.take() else {
                platform::show_error("NSO Album Sync", "Tray icon initialization state was lost.");
                *control_flow = ControlFlow::Exit;
                return;
            };
            match TrayIconBuilder::new()
                .with_menu(Box::new(tray_menu.clone()))
                .with_tooltip("NSO Album Sync")
                .with_icon(icon)
                .build()
            {
                Ok(icon) => tray_icon = Some(icon),
                Err(error) => {
                    platform::show_error("NSO Album Sync", &format!("Could not create tray icon: {error}"));
                    *control_flow = ControlFlow::Exit;
                    return;
                }
            }
        }

        while let Ok(event) = receiver.try_recv() {
            match event {
                AppEvent::Sync(event) => {
                    sync_running = false;
                    match event.result {
                        Ok(result) => {
                            let now = chrono::Local::now().format("%H:%M (%Y-%m-%d)").to_string();
                            let _ = config.update(|value| value.set_last_sync(now));
                            let current = config.snapshot();
                            menu.refresh(&current, platform::start_on_boot_enabled(), "Ready");
                            if current.notifications() {
                                if result.new_downloads() > 0 {
                                    let noun = if result.new_downloads() == 1 { "capture" } else { "captures" };
                                    platform::notify(
                                        "NSO Album Sync",
                                        &format!("Synced {} new {noun} to your album folder!", result.new_downloads()),
                                    );
                                } else if !event.background {
                                    platform::notify(
                                        "NSO Album Sync",
                                        "Album is up to date. No new captures found.",
                                    );
                                }
                            }
                        }
                        Err(error) => {
                            if is_invalid_grant(&error) {
                                clear_account_state(AccountStateRefs {
                                    config: &config,
                                    auth: auth.as_ref(),
                                    coral: coral.as_ref(),
                                    splatnet: splatnet.as_ref(),
                                    game_services: game_services.as_ref(),
                                    zelda: zelda.as_ref(),
                                    discord: discord.as_ref(),
                                    enrichment_state: enrichment_state.as_ref(),
                                });
                                initial_sync_deferred = false;
                                menu.refresh(
                                    &config.snapshot(),
                                    platform::start_on_boot_enabled(),
                                    "Nintendo Account session expired. Sign in again to continue.",
                                );
                            } else {
                                menu.set_status(&format!("Sync failed: {error}"));
                                if config.snapshot().notifications() {
                                    platform::notify("NSO Album Sync", &error.to_string());
                                }
                            }
                        }
                    }
                }
                AppEvent::Auth(result) => {
                    auth_running = false;
                    match result {
                        Ok((token, nickname)) => {
                            auth_pending = false;
                            let _ = config.update(|value| value.set_session(token, nickname));
                            coral.clear_cached_session();
                            splatnet.clear_cache();
                            game_services.clear_cache();
                            zelda.clear_cache();
                            reset_presence_state(enrichment_state.as_ref());
                            let current = config.snapshot();
                            menu.refresh(
                                &current,
                                platform::start_on_boot_enabled(),
                                &format!("Connected as {}", current.user_nickname()),
                            );
                            initial_sync_deferred =
                                current.auto_sync() && current.discord_presence();
                            if current.discord_presence() {
                                next_presence = Instant::now();
                                next_auto_sync = jittered_deadline(sync_interval(&current));
                            } else if current.auto_sync() {
                                next_auto_sync = Instant::now();
                            }
                        }
                        Err(error) => {
                            if !error.to_string().contains("invalid OAuth state") {
                                auth_pending = false;
                            }
                            menu.set_status("Sign in failed");
                            platform::show_error("Nintendo Account sign in", &error.to_string());
                        }
                    }
                }
                AppEvent::PresenceBasic(event) => {
                    let current = config.snapshot();
                    let same_account = current.session_token() == event.session_token;
                    if same_account {
                        match event.result {
                            Ok(presence) => {
                                if current.discord_presence() && presence.is_playing() {
                                    discord.update(presence.as_ref(), &zelda.live_presence());
                                    menu.set_status(presence.game_name());
                                } else {
                                    discord.clear();
                                    menu.set_status("Ready");
                                }
                                release_initial_sync(
                                    &mut initial_sync_deferred,
                                    &mut next_auto_sync,
                                    &config,
                                );
                            }
                            Err(error) => {
                                if is_invalid_grant(&error) {
                                    clear_account_state(AccountStateRefs {
                                        config: &config,
                                        auth: auth.as_ref(),
                                        coral: coral.as_ref(),
                                        splatnet: splatnet.as_ref(),
                                        game_services: game_services.as_ref(),
                                        zelda: zelda.as_ref(),
                                        discord: discord.as_ref(),
                                        enrichment_state: enrichment_state.as_ref(),
                                    });
                                    initial_sync_deferred = false;
                                    menu.refresh(
                                        &config.snapshot(),
                                        platform::start_on_boot_enabled(),
                                        "Nintendo Account session expired. Sign in again to continue.",
                                    );
                                } else {
                                    release_initial_sync(
                                        &mut initial_sync_deferred,
                                        &mut next_auto_sync,
                                        &config,
                                    );
                                    menu.set_status(&format!("Presence error: {error}"));
                                }
                            }
                        }
                    }
                    if !event.final_expected {
                        presence_running = false;
                        next_presence = jittered_deadline(PRESENCE_POLL_INTERVAL);
                    }
                }
                AppEvent::PresenceFinal(event) => {
                    presence_running = false;
                    let current = config.snapshot();
                    if current.discord_presence()
                        && current.session_token() == event.session_token
                        && event.presence.is_playing()
                    {
                        discord.update(event.presence.as_ref(), &zelda.live_presence());
                    }
                    next_presence = jittered_deadline(PRESENCE_POLL_INTERVAL);
                }
            }
        }

        let current_zelda = zelda.live_presence();
        if current_zelda != last_zelda {
            last_zelda = current_zelda.clone();
            if config.snapshot().discord_presence() {
                discord.refresh_zelda(&current_zelda);
            }
        }

        if !auth_running
            && auth_pending
            && let Ok(Some(callback)) = take_callback()
        {
            auth_running = true;
            menu.set_status("Completing Nintendo Account sign-in…");
            spawn_auth_completion(Arc::clone(&auth), sender.clone(), callback);
        }

        let snapshot = config.snapshot();
        if snapshot.auto_sync()
            && !snapshot.session_token().is_empty()
            && !sync_running
            && Instant::now() >= next_auto_sync
            && !initial_sync_deferred
        {
            sync_running = true;
            menu.set_status("Syncing album…");
            spawn_sync(
                Arc::clone(&sync_engine),
                Arc::clone(&stop),
                sender.clone(),
                true,
            );
            next_auto_sync = jittered_deadline(sync_interval(&snapshot));
        }
        if snapshot.discord_presence()
            && !snapshot.session_token().is_empty()
            && !presence_running
            && Instant::now() >= next_presence
        {
            presence_running = true;
            spawn_presence(
                PresenceDependencies {
                    auth: Arc::clone(&auth),
                    coral: Arc::clone(&coral),
                    splatnet: Arc::clone(&splatnet),
                    game_services: Arc::clone(&game_services),
                    zelda: Arc::clone(&zelda),
                },
                Arc::clone(&enrichment_state),
                sender.clone(),
                snapshot.session_token().to_owned(),
            );
        }

        while let Ok(menu_event) = MenuEvent::receiver().try_recv() {
            if menu.matches(&menu_event, &menu.exit) {
                stop.store(true, Ordering::Release);
                zelda.stop_live_session();
                discord.clear();
                *control_flow = ControlFlow::Exit;
                return;
            }
            if menu.matches(&menu_event, &menu.sign_in) {
                if snapshot.session_token().is_empty() {
                    if auth_pending {
                        platform::notify(
                            "Nintendo Sign-In",
                            "A Nintendo Account sign-in is already waiting in your browser.",
                        );
                    } else if !platform::confirm(NXAPI_DISCLOSURE_TITLE, NXAPI_DISCLOSURE) {
                        menu.set_status("Sign-in cancelled");
                    } else {
                        match auth.authorize_url() {
                            Ok(url) => {
                                auth_pending = true;
                                menu.set_status("Waiting for Nintendo Account sign-in in your browser…");
                                if let Err(error) = open::that(url) {
                                    auth_pending = false;
                                    platform::show_error(
                                        "Nintendo Account sign in",
                                        &error.to_string(),
                                    );
                                }
                            }
                            Err(error) => platform::show_error(
                                "Nintendo Account sign in",
                                &error.to_string(),
                            ),
                        }
                    }
                } else {
                    auth_pending = false;
                    initial_sync_deferred = false;
                    clear_account_state(AccountStateRefs {
                        config: &config,
                        auth: auth.as_ref(),
                        coral: coral.as_ref(),
                        splatnet: splatnet.as_ref(),
                        game_services: game_services.as_ref(),
                        zelda: zelda.as_ref(),
                        discord: discord.as_ref(),
                        enrichment_state: enrichment_state.as_ref(),
                    });
                    menu.refresh(
                        &config.snapshot(),
                        platform::start_on_boot_enabled(),
                        "Signed out",
                    );
                    if snapshot.notifications() {
                        platform::notify(
                            "NSO Album Sync",
                            "Signed out of your Nintendo Account.",
                        );
                    }
                }
            } else if menu.matches(&menu_event, &menu.sync_now) && !sync_running {
                if snapshot.session_token().is_empty() {
                    platform::show_info(
                        "NSO Album Sync",
                        "Sign in to your Nintendo Account first.",
                    );
                } else {
                    initial_sync_deferred = false;
                    sync_running = true;
                    menu.set_status("Syncing album…");
                    spawn_sync(
                        Arc::clone(&sync_engine),
                        Arc::clone(&stop),
                        sender.clone(),
                        false,
                    );
                    if snapshot.discord_presence() {
                        next_presence = Instant::now();
                    }
                }
            } else if menu.matches(&menu_event, &menu.auto_sync) {
                let updated = config.update(AppConfig::toggle_auto_sync).ok();
                initial_sync_deferred = false;
                if let Some(updated) = updated {
                    if updated.auto_sync()
                        && !updated.session_token().is_empty()
                        && !sync_running
                    {
                        sync_running = true;
                        menu.set_status("Syncing album…");
                        spawn_sync(
                            Arc::clone(&sync_engine),
                            Arc::clone(&stop),
                            sender.clone(),
                            false,
                        );
                        next_auto_sync = jittered_deadline(sync_interval(&updated));
                    }
                    if updated.notifications() {
                        platform::notify(
                            "NSO Album Sync",
                            if updated.auto_sync() {
                                "Auto-sync enabled."
                            } else {
                                "Auto-sync disabled."
                            },
                        );
                    }
                }
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.notifications) {
                let _ = config.update(AppConfig::toggle_notifications);
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.discord) {
                let updated = config.update(AppConfig::toggle_discord_presence).ok();
                if updated.as_ref().is_some_and(AppConfig::discord_presence) {
                    next_presence = Instant::now();
                    if updated.as_ref().is_some_and(AppConfig::notifications) {
                        platform::notify(
                            "Discord Rich Presence",
                            "Presence is enabled. Discord Activity Sharing must also be enabled for other people to see it.",
                        );
                    }
                } else {
                    release_initial_sync(
                        &mut initial_sync_deferred,
                        &mut next_auto_sync,
                        &config,
                    );
                    reset_presence_state(enrichment_state.as_ref());
                    zelda.stop_live_session();
                    discord.clear();
                }
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.start_boot) {
                let enabled = !platform::start_on_boot_enabled();
                match platform::set_start_on_boot(enabled) {
                    Ok(()) => {
                        let _ = config.update(|value| value.set_start_on_boot(enabled));
                    }
                    Err(error) => platform::show_error("Start on boot", &error.to_string()),
                }
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.choose_folder) {
                let current = std::path::PathBuf::from(snapshot.destination_folder());
                if let Some(folder) = platform::choose_folder(&current) {
                    let _ = config.update(|value| {
                        value.set_destination_folder(folder.to_string_lossy().into_owned());
                    });
                    menu.refresh(
                        &config.snapshot(),
                        platform::start_on_boot_enabled(),
                        "Album folder updated",
                    );
                }
            } else if menu.matches(&menu_event, &menu.open_folder) {
                if let Err(error) =
                    platform::open_folder(std::path::Path::new(snapshot.destination_folder()))
                {
                    platform::show_error("Open album folder", &error.to_string());
                }
            } else if menu.matches(&menu_event, &menu.proxy_settings) {
                if let Ok(directory) = config_directory() {
                    let _ = platform::open_folder(&directory);
                }
                platform::show_info(
                    "Proxy settings",
                    "Edit proxyUrl in config.json, or run with --proxy URL / --clear-proxy, then restart NSO Album Sync.",
                );
            } else if menu.matches(&menu_event, &menu.about) {
                platform::show_info(
                    "About NSO Album Sync",
                    &format!(
                        "NSO Album Sync {}\n\nSync Nintendo Switch Online album captures to your PC and publish optional Discord Rich Presence.\n\nSafe Rust port — unsafe code is forbidden at the crate and Cargo lint levels.",
                        env!("CARGO_PKG_VERSION")
                    ),
                );
            } else if let Some(minutes) = menu.interval_for(&menu_event) {
                let _ = config.update(|value| value.set_sync_interval_minutes(minutes));
                next_auto_sync =
                    jittered_deadline(Duration::from_secs(u64::from(minutes) * 60));
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            }
        }
    });
}

struct SyncEvent {
    result: anyhow::Result<SyncResult>,
    background: bool,
}

struct PresenceBasicEvent {
    session_token: String,
    result: anyhow::Result<Box<NintendoPresence>>,
    final_expected: bool,
}

struct PresenceFinalEvent {
    session_token: String,
    presence: Box<NintendoPresence>,
}

enum AppEvent {
    Sync(SyncEvent),
    Auth(anyhow::Result<(String, String)>),
    PresenceBasic(PresenceBasicEvent),
    PresenceFinal(PresenceFinalEvent),
}

fn spawn_auth_completion(
    auth: Arc<NintendoAuthManager>,
    sender: mpsc::Sender<AppEvent>,
    callback: String,
) {
    std::thread::spawn(move || {
        let result = auth.complete_login(&callback).map(|auth_result| {
            (
                auth_result.session_token().to_owned(),
                auth_result.user_nickname().to_owned(),
            )
        });
        let _ = sender.send(AppEvent::Auth(result));
    });
}

fn spawn_sync(
    engine: Arc<SyncEngine>,
    stop: Arc<AtomicBool>,
    sender: mpsc::Sender<AppEvent>,
    background: bool,
) {
    std::thread::spawn(move || {
        let result = engine.sync(|| stop.load(Ordering::Acquire));
        let _ = sender.send(AppEvent::Sync(SyncEvent { result, background }));
    });
}

struct PresenceDependencies {
    auth: Arc<NintendoAuthManager>,
    coral: Arc<CoralClient>,
    splatnet: Arc<SplatNetClient>,
    game_services: Arc<GameServicesClient>,
    zelda: Arc<ZeldaNotesClient>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RpcGameService {
    None,
    Splatoon3,
    ZeldaNotes,
    AnimalCrossing,
    Splatoon2,
}

#[derive(Debug, Clone, Default)]
struct EnrichmentFields {
    details: String,
    state: String,
    image_uri: String,
}

impl EnrichmentFields {
    fn from_presence(presence: &NintendoPresence) -> Self {
        Self {
            details: presence.custom_details().to_owned(),
            state: presence.custom_state().to_owned(),
            image_uri: presence.custom_image_uri().to_owned(),
        }
    }

    fn apply(&self, presence: &mut NintendoPresence) {
        presence.set_custom_details(self.details.clone());
        presence.set_custom_state(self.state.clone());
        presence.set_custom_image_uri(self.image_uri.clone());
    }
}

#[derive(Debug, Default)]
struct PresenceEnrichmentState {
    active_game_key: String,
    attempted: bool,
    cached: EnrichmentFields,
}

impl PresenceEnrichmentState {
    fn reset(&mut self) {
        self.active_game_key.clear();
        self.attempted = false;
        self.cached = EnrichmentFields::default();
    }

    fn plan(&mut self, game_key: &str, service: RpcGameService) -> EnrichmentPlan {
        if self.active_game_key != game_key {
            self.active_game_key = game_key.to_owned();
            self.attempted = false;
            self.cached = EnrichmentFields::default();
        }
        if service == RpcGameService::None {
            return EnrichmentPlan::Skip;
        }
        if self.attempted {
            EnrichmentPlan::Reuse(self.cached.clone())
        } else {
            self.attempted = true;
            EnrichmentPlan::Fetch
        }
    }

    fn store(&mut self, game_key: &str, presence: &NintendoPresence) {
        if self.active_game_key == game_key {
            self.cached = EnrichmentFields::from_presence(presence);
        }
    }
}

enum EnrichmentPlan {
    Skip,
    Reuse(EnrichmentFields),
    Fetch,
}

fn spawn_presence(
    deps: PresenceDependencies,
    state: Arc<Mutex<PresenceEnrichmentState>>,
    sender: mpsc::Sender<AppEvent>,
    session_token: String,
) {
    std::thread::spawn(move || {
        let mut presence = match deps.coral.self_presence(&session_token) {
            Ok(presence) => presence,
            Err(error) => {
                let _ = sender.send(AppEvent::PresenceBasic(PresenceBasicEvent {
                    session_token,
                    result: Err(error),
                    final_expected: false,
                }));
                return;
            }
        };

        if !presence.is_playing() {
            reset_presence_state(state.as_ref());
            deps.zelda.set_active_game(ZeldaGame::Unknown);
            let _ = sender.send(AppEvent::PresenceBasic(PresenceBasicEvent {
                session_token,
                result: Ok(Box::new(presence)),
                final_expected: false,
            }));
            return;
        }

        let service = rpc_game_service_for(&presence);
        let game_key = if presence.title_id().is_empty() {
            presence.game_name().to_owned()
        } else {
            presence.title_id().to_owned()
        };
        if service != RpcGameService::ZeldaNotes {
            deps.zelda.set_active_game(ZeldaGame::Unknown);
        }
        let plan = state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .plan(&game_key, service);
        let final_expected = !matches!(plan, EnrichmentPlan::Skip);
        if sender
            .send(AppEvent::PresenceBasic(PresenceBasicEvent {
                session_token: session_token.clone(),
                result: Ok(Box::new(presence.clone())),
                final_expected,
            }))
            .is_err()
        {
            return;
        }

        match plan {
            EnrichmentPlan::Skip => return,
            EnrichmentPlan::Reuse(cached) => cached.apply(&mut presence),
            EnrichmentPlan::Fetch => {
                enrich_presence(&deps, &session_token, service, &mut presence);
                state
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .store(&game_key, &presence);
            }
        }
        let _ = sender.send(AppEvent::PresenceFinal(PresenceFinalEvent {
            session_token,
            presence: Box::new(presence),
        }));
    });
}

fn enrich_presence(
    deps: &PresenceDependencies,
    session_token: &str,
    service: RpcGameService,
    presence: &mut NintendoPresence,
) {
    if let Ok(tokens) = deps.auth.exchange_session_token(session_token)
        && let Ok(profile) = deps.auth.fetch_profile(tokens.access_token())
    {
        deps.splatnet.set_locale(profile.language(), profile.country());
        deps.game_services.set_locale(profile.language(), profile.country());
        deps.zelda.set_locale(profile.language(), profile.country());
    }

    match service {
        RpcGameService::Splatoon3 => {
            if let Ok(token) = service_token_with_fallback(
                deps.coral.as_ref(),
                session_token,
                SPLATNET3_GAME_SERVICE_ID,
                SPLATNET3_GAME_SERVICE_ID_ALT,
            ) && let Ok(extra) = deps.splatnet.fetch_presence(&token)
                && extra.active()
            {
                presence.set_custom_details(extra.format_details());
                presence.set_custom_state(extra.format_state());
                if !extra.stage_image_uri().is_empty() {
                    presence.set_custom_image_uri(extra.stage_image_uri().to_owned());
                }
            }
        }
        RpcGameService::ZeldaNotes => {
            let game = game_for_presence(presence.title_id(), presence.game_name());
            if let Ok(token) = service_token_with_fallback(
                deps.coral.as_ref(),
                session_token,
                ZELDA_NOTES_GAME_SERVICE_ID,
                ZELDA_NOTES_GAME_SERVICE_ID_ALT,
            ) && let Ok(extra) = deps.zelda.fetch_presence(&token)
            {
                deps.zelda.set_active_game(game);
                if extra.active() {
                    if !extra.format_details().is_empty() {
                        presence.set_custom_details(extra.format_details().to_owned());
                    }
                    if !extra.format_state().is_empty() {
                        presence.set_custom_state(extra.format_state().to_owned());
                    }
                    if !extra.stage_image_uri().is_empty() {
                        presence.set_custom_image_uri(extra.stage_image_uri().to_owned());
                    }
                }
            }
        }
        RpcGameService::AnimalCrossing => {
            if let Ok(token) = deps
                .coral
                .get_web_service_token(session_token, ANIMAL_CROSSING_GAME_SERVICE_ID)
                && let Ok(extra) = deps.game_services.fetch_animal_crossing_presence(&token)
                && extra.active()
            {
                presence.set_custom_details(extra.format_details());
                presence.set_custom_state(extra.format_state());
                if !extra.image_uri().is_empty() && extra.image_uri().len() <= 300 {
                    presence.set_custom_image_uri(extra.image_uri().to_owned());
                }
            }
        }
        RpcGameService::Splatoon2 => {
            if let Ok(token) = deps
                .coral
                .get_web_service_token(session_token, SPLATOON2_GAME_SERVICE_ID)
                && let Ok(extra) = deps.game_services.fetch_splatoon2_presence(&token)
                && extra.active()
            {
                presence.set_custom_details(extra.format_details());
                presence.set_custom_state(extra.format_state());
                if !extra.stage_image_uri().is_empty() {
                    presence.set_custom_image_uri(extra.stage_image_uri().to_owned());
                }
            }
        }
        RpcGameService::None => {}
    }
}

fn service_token_with_fallback(
    coral: &CoralClient,
    session: &str,
    primary: u64,
    alternate: u64,
) -> anyhow::Result<String> {
    coral
        .get_web_service_token(session, primary)
        .or_else(|_| coral.get_web_service_token(session, alternate))
}

fn rpc_game_service_for(presence: &NintendoPresence) -> RpcGameService {
    match presence.title_id() {
        "0100c2500fc20000" => return RpcGameService::Splatoon3,
        "01003bc0000a0000" => return RpcGameService::Splatoon2,
        "01006f8002326000" => return RpcGameService::AnimalCrossing,
        "01007ef00011e000" | "0100f2c0115b6000" => return RpcGameService::ZeldaNotes,
        _ => {}
    }
    let name = presence.game_name();
    if contains_any(name, &["Splatoon 3", "スプラトゥーン3"]) {
        RpcGameService::Splatoon3
    } else if contains_any(
        name,
        &[
            "Breath of the Wild",
            "ブレス オブ ザ ワイルド",
            "Tears of the Kingdom",
            "ティアーズ オブ ザ キングダム",
        ],
    ) {
        RpcGameService::ZeldaNotes
    } else if contains_any(
        name,
        &["Animal Crossing", "New Horizons", "どうぶつの森", "あつ森"],
    ) {
        RpcGameService::AnimalCrossing
    } else if contains_any(name, &["Splatoon 2", "スプラトゥーン2"]) {
        RpcGameService::Splatoon2
    } else {
        RpcGameService::None
    }
}

fn contains_any(text: &str, needles: &[&str]) -> bool {
    needles.iter().any(|needle| text.contains(needle))
}

fn reset_presence_state(state: &Mutex<PresenceEnrichmentState>) {
    state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .reset();
}

fn release_initial_sync(
    deferred: &mut bool,
    next_auto_sync: &mut Instant,
    config: &ConfigManager,
) {
    if !*deferred {
        return;
    }
    *deferred = false;
    let current = config.snapshot();
    if current.auto_sync() && !current.session_token().is_empty() {
        *next_auto_sync = Instant::now();
    }
}

struct AccountStateRefs<'a> {
    config: &'a ConfigManager,
    auth: &'a NintendoAuthManager,
    coral: &'a CoralClient,
    splatnet: &'a SplatNetClient,
    game_services: &'a GameServicesClient,
    zelda: &'a ZeldaNotesClient,
    discord: &'a DiscordPresence,
    enrichment_state: &'a Mutex<PresenceEnrichmentState>,
}

fn clear_account_state(deps: AccountStateRefs<'_>) {
    let _ = deps.config.clear_session();
    deps.auth.clear_cached_tokens();
    deps.coral.clear_cached_session();
    deps.splatnet.clear_cache();
    deps.game_services.clear_cache();
    deps.zelda.clear_cache();
    deps.zelda.stop_live_session();
    deps.discord.clear();
    reset_presence_state(deps.enrichment_state);
}

fn is_invalid_grant(error: &anyhow::Error) -> bool {
    error.to_string().contains("invalid_grant")
}

fn apply_cli_settings(config: &ConfigManager, args: &[String]) -> anyhow::Result<()> {
    if let Some(index) = args.iter().position(|argument| argument == "--proxy") {
        let value = args
            .get(index + 1)
            .ok_or_else(|| anyhow::anyhow!("--proxy requires a URL"))?
            .clone();
        let parsed = url::Url::parse(&value)?;
        anyhow::ensure!(
            matches!(parsed.scheme(), "http" | "https" | "socks5" | "socks5h"),
            "proxy URL uses an unsupported scheme"
        );
        config.update(|config| config.set_proxy_url(value))?;
    }
    if args.iter().any(|argument| argument == "--clear-proxy") {
        config.update(|config| config.set_proxy_url(String::new()))?;
    }
    Ok(())
}

#[derive(Clone)]
struct TrayMenu {
    root: Menu,
    nickname: MenuItem,
    last_sync: MenuItem,
    status: MenuItem,
    sign_in: MenuItem,
    sync_now: MenuItem,
    auto_sync: MenuItem,
    notifications: MenuItem,
    discord: MenuItem,
    start_boot: MenuItem,
    interval_15: MenuItem,
    interval_30: MenuItem,
    interval_60: MenuItem,
    interval_120: MenuItem,
    interval_240: MenuItem,
    choose_folder: MenuItem,
    open_folder: MenuItem,
    proxy_settings: MenuItem,
    about: MenuItem,
    exit: MenuItem,
}

impl TrayMenu {
    fn new(config: &AppConfig, start_on_boot: bool) -> anyhow::Result<Self> {
        let root = Menu::new();
        let menu = Self {
            root: root.clone(),
            nickname: MenuItem::with_id("nickname", "", false, None),
            last_sync: MenuItem::with_id("last_sync", "", false, None),
            status: MenuItem::with_id("status", "", false, None),
            sign_in: MenuItem::with_id("sign_in", "", true, None),
            sync_now: MenuItem::with_id("sync_now", "Sync now", true, None),
            auto_sync: MenuItem::with_id("auto_sync", "", true, None),
            notifications: MenuItem::with_id("notifications", "", true, None),
            discord: MenuItem::with_id("discord", "", true, None),
            start_boot: MenuItem::with_id("start_boot", "", true, None),
            interval_15: MenuItem::with_id("interval_15", "15 minutes", true, None),
            interval_30: MenuItem::with_id("interval_30", "30 minutes", true, None),
            interval_60: MenuItem::with_id("interval_60", "60 minutes", true, None),
            interval_120: MenuItem::with_id("interval_120", "2 hours", true, None),
            interval_240: MenuItem::with_id("interval_240", "4 hours", true, None),
            choose_folder: MenuItem::with_id("choose_folder", "Choose album folder…", true, None),
            open_folder: MenuItem::with_id("open_folder", "Open album folder", true, None),
            proxy_settings: MenuItem::with_id("proxy_settings", "Proxy settings…", true, None),
            about: MenuItem::with_id("about", "About NSO Album Sync…", true, None),
            exit: MenuItem::with_id("exit", "Exit", true, None),
        };
        for item in [
            &menu.nickname,
            &menu.last_sync,
            &menu.status,
            &menu.sign_in,
            &menu.sync_now,
            &menu.auto_sync,
            &menu.notifications,
            &menu.discord,
            &menu.start_boot,
            &menu.interval_15,
            &menu.interval_30,
            &menu.interval_60,
            &menu.interval_120,
            &menu.interval_240,
            &menu.choose_folder,
            &menu.open_folder,
            &menu.proxy_settings,
            &menu.about,
            &menu.exit,
        ] {
            root.append(item)?;
        }
        menu.refresh(config, start_on_boot, "Ready");
        Ok(menu)
    }

    fn refresh(&self, config: &AppConfig, start_on_boot: bool, status: &str) {
        self.nickname
            .set_text(format!("User: {}", config.user_nickname()));
        self.last_sync
            .set_text(format!("Last sync: {}", config.last_sync()));
        self.status.set_text(format!("Status: {status}"));
        self.sign_in.set_text(if config.session_token().is_empty() {
            "Sign in to Nintendo Account…"
        } else {
            "Sign out of Nintendo Account"
        });
        self.auto_sync
            .set_text(toggle_text("Auto sync", config.auto_sync()));
        self.notifications
            .set_text(toggle_text("Notifications", config.notifications()));
        self.discord
            .set_text(toggle_text("Discord Rich Presence", config.discord_presence()));
        self.start_boot
            .set_text(toggle_text("Start on boot", start_on_boot));
        self.sync_now.set_enabled(!config.session_token().is_empty());
    }

    fn set_status(&self, status: &str) {
        self.status.set_text(format!("Status: {status}"));
    }

    fn matches(&self, event: &MenuEvent, item: &MenuItem) -> bool {
        event.id == item.id()
    }

    fn interval_for(&self, event: &MenuEvent) -> Option<u32> {
        [
            (15, &self.interval_15),
            (30, &self.interval_30),
            (60, &self.interval_60),
            (120, &self.interval_120),
            (240, &self.interval_240),
        ]
        .into_iter()
        .find_map(|(minutes, item)| self.matches(event, item).then_some(minutes))
    }
}

fn toggle_text(label: &str, enabled: bool) -> String {
    format!("{} {label}", if enabled { "✓" } else { " " })
}

fn sync_interval(config: &AppConfig) -> Duration {
    Duration::from_secs(u64::from(config.sync_interval_minutes().max(1)) * 60)
}

fn jittered_deadline(interval: Duration) -> Instant {
    let nominal_millis = interval.as_millis().min(u64::MAX as u128) as u64;
    let jitter = (nominal_millis / 50).max(1);
    let offset = rand::rng().random_range(-(jitter as i64)..=(jitter as i64));
    let adjusted = if offset.is_negative() {
        nominal_millis.saturating_sub(offset.unsigned_abs())
    } else {
        nominal_millis.saturating_add(offset as u64)
    };
    Instant::now() + Duration::from_millis(adjusted.max(1))
}

fn app_icon() -> anyhow::Result<Icon> {
    let mut rgba = Vec::with_capacity(32 * 32 * 4);
    for y in 0..32 {
        for x in 0..32 {
            let inside = (5..27).contains(&x) && (5..27).contains(&y);
            if inside {
                rgba.extend_from_slice(&[230, 35, 80, 255]);
            } else {
                rgba.extend_from_slice(&[0, 0, 0, 0]);
            }
        }
    }
    Ok(Icon::from_rgba(rgba, 32, 32)?)
}
