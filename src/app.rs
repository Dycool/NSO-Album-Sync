//! Desktop application orchestration and tray event loop.

use crate::auth_callback::{
    callback_from_args, clear_callback, publish_callback, register_protocol, take_callback,
    unregister_protocol,
};
use crate::config::{ConfigManager, config_directory};
use crate::coral::CoralClient;
use crate::discord::DiscordPresence;
use crate::game_services::{
    ANIMAL_CROSSING_GAME_SERVICE_ID, GameServicesClient, SPLATOON2_GAME_SERVICE_ID,
};
use crate::http::HttpClient;
use crate::model::{AppConfig, NintendoPresence, SyncResult};
use crate::nintendo_auth::NintendoAuthManager;
use crate::nxapi::NxapiClient;
use crate::platform;
use crate::single_instance::SingleInstance;
use crate::splatnet::{
    SPLATNET3_GAME_SERVICE_ID, SPLATNET3_GAME_SERVICE_ID_ALT, SplatNetClient,
};
use crate::sync::SyncEngine;
use crate::zelda_notes::{
    ZELDA_NOTES_GAME_SERVICE_ID, ZELDA_NOTES_GAME_SERVICE_ID_ALT, ZeldaNotesClient,
    game_for_presence,
};
use crate::zelda_regions::ZeldaGame;
use rand::Rng as _;
#[cfg(target_os = "macos")]
use std::cell::Cell;
use std::io::Cursor;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};
use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoop};
use tray_icon::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem};
#[cfg(target_os = "windows")]
use tray_icon::{MouseButton, TrayIconEvent};
use tray_icon::{Icon, TrayIconBuilder};

const PRESENCE_POLL_INTERVAL: Duration = Duration::from_secs(60);
const EXIT_WATCHDOG_DELAY: Duration = Duration::from_secs(5);
const NXAPI_DISCLOSURE_TITLE: &str = "Third-Party Service Disclosure";
const NXAPI_DISCLOSURE: &str = "NSO Album Sync uses the third-party nxapi-znca-api service at fancy.org.uk for Nintendo Switch Online request attestation and request/response encryption.\n\nWhen Nintendo Switch Online features are used, your Nintendo Account id_token and the profile fields required by Coral (Nintendo Account ID, birthday, country, and language), your Coral access token, and the Coral API requests and responses used by this app are sent to and processed by that third-party service. These tokens can authenticate Nintendo services while they remain valid.\n\nContinue with Nintendo Account sign-in?";
const CALLBACK_REGISTRATION_ERROR: &str = "Automatic Nintendo Account browser return could not be registered. Close any other app using the Nintendo Switch App sign-in link and try again.";

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

    let config = ConfigManager::load()?;
    apply_cli_settings(&config, &args)?;
    let initial = config.snapshot();
    let http = HttpClient::new(initial.proxy_url().to_owned());
    let shared_http = http.clone();
    let auth = Arc::new(NintendoAuthManager::new(http.clone()));
    let nxapi_cache = config_directory()?.join("nxapi-cache.json");
    let nxapi = Arc::new(NxapiClient::new(
        http.clone(),
        initial.nxapi_auth_client_id().to_owned(),
        nxapi_cache,
    ));
    let coral = Arc::new(CoralClient::new(
        http.clone(),
        Arc::clone(&auth),
        Arc::clone(&nxapi),
    ));
    let splatnet = Arc::new(SplatNetClient::new(http.clone()));
    let game_services = Arc::new(GameServicesClient::new(http.clone()));
    let zelda = Arc::new(ZeldaNotesClient::new(http.clone()));
    let sync_engine = Arc::new(SyncEngine::new(config.clone(), Arc::clone(&coral), http));
    let discord = Arc::new(DiscordPresence::new(initial.discord_application_id()));
    let enrichment_state = Arc::new(Mutex::new(PresenceEnrichmentState::default()));
    let account_generation = Arc::new(AtomicU64::new(0));

    let (sender, receiver) = mpsc::channel::<AppEvent>();
    let stop = Arc::new(AtomicBool::new(false));
    let mut sync_running = false;
    let mut auth_running = false;
    let mut auth_pending = false;
    let mut presence_running = false;
    let mut presence_refresh_requested = false;
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
        *control_flow = ControlFlow::WaitUntil(Instant::now() + Duration::from_millis(150));
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
                .with_menu_on_left_click(!cfg!(target_os = "windows"))
                .with_menu_on_right_click(true)
                .with_icon_as_template(cfg!(target_os = "macos"))
                .with_icon(icon)
                .build()
            {
                Ok(icon) => tray_icon = Some(icon),
                Err(error) => {
                    platform::show_error(
                        "NSO Album Sync",
                        &format!("Could not create tray icon: {error}"),
                    );
                    *control_flow = ControlFlow::Exit;
                    return;
                }
            }
            if config.snapshot().session_token().is_empty() {
                begin_sign_in(auth.as_ref(), &menu, &mut auth_pending);
            }
        }

        while let Ok(app_event) = receiver.try_recv() {
            match app_event {
                AppEvent::Sync(event) => {
                    if event.generation != account_generation.load(Ordering::Acquire) {
                        continue;
                    }
                    sync_running = false;
                    match event.result {
                        Ok(result) => {
                            let now = chrono::Local::now().format("%H:%M (%Y-%m-%d)").to_string();
                            let _ = config.update(|value| value.set_last_sync(now));
                            let current = config.snapshot();
                            menu.refresh(&current, platform::start_on_boot_enabled(), "Ready");
                            if current.notifications() {
                                if result.new_downloads() > 0 {
                                    let noun = if result.new_downloads() == 1 {
                                        "capture"
                                    } else {
                                        "captures"
                                    };
                                    platform::notify(
                                        "NSO Album Sync",
                                        &format!(
                                            "Synced {} new {noun} to your album folder!",
                                            result.new_downloads()
                                        ),
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
                                presence_running = false;
                                presence_refresh_requested = false;
                                clear_account_state(AccountStateRefs {
                                    config: &config,
                                    auth: auth.as_ref(),
                                    nxapi: nxapi.as_ref(),
                                    coral: coral.as_ref(),
                                    splatnet: splatnet.as_ref(),
                                    game_services: game_services.as_ref(),
                                    zelda: zelda.as_ref(),
                                    discord: discord.as_ref(),
                                    enrichment_state: enrichment_state.as_ref(),
                                    generation: account_generation.as_ref(),
                                });
                                initial_sync_deferred = false;
                                menu.refresh(
                                    &config.snapshot(),
                                    platform::start_on_boot_enabled(),
                                    "Nintendo Account session expired. Sign in again to continue.",
                                );
                            } else {
                                menu.set_status(&error.to_string());
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
                            unregister_protocol();
                            let _ = clear_callback();
                            sync_running = false;
                            presence_running = false;
                            presence_refresh_requested = false;
                            let _ = account_generation.fetch_add(1, Ordering::AcqRel);
                            nxapi.clear_user_auth();
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
                            let message = error.to_string();
                            if !message.contains("invalid OAuth state") {
                                auth_pending = false;
                                unregister_protocol();
                                let _ = clear_callback();
                            }
                            menu.set_status(&message);
                            if config.snapshot().notifications() {
                                platform::notify("Nintendo Sign-In", &message);
                            }
                        }
                    }
                }
                AppEvent::PresenceBasic(event) => {
                    let current = config.snapshot();
                    if current.session_token() != event.session_token {
                        continue;
                    }
                    match event.result {
                        Ok(presence) => {
                            if current.discord_presence() && presence.is_playing() {
                                discord.update(presence.as_ref(), &zelda.live_presence());
                            } else {
                                discord.clear();
                            }
                            release_initial_sync(
                                &mut initial_sync_deferred,
                                &mut next_auto_sync,
                                &config,
                            );
                        }
                        Err(error) => {
                            if is_invalid_grant(&error) {
                                sync_running = false;
                                presence_running = false;
                                presence_refresh_requested = false;
                                clear_account_state(AccountStateRefs {
                                    config: &config,
                                    auth: auth.as_ref(),
                                    nxapi: nxapi.as_ref(),
                                    coral: coral.as_ref(),
                                    splatnet: splatnet.as_ref(),
                                    game_services: game_services.as_ref(),
                                    zelda: zelda.as_ref(),
                                    discord: discord.as_ref(),
                                    enrichment_state: enrichment_state.as_ref(),
                                    generation: account_generation.as_ref(),
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
                                eprintln!("Presence: {error}");
                            }
                        }
                    }
                    if !event.final_expected {
                        presence_running = false;
                        next_presence = presence_deadline(&mut presence_refresh_requested);
                    }
                }
                AppEvent::PresenceFinal(event) => {
                    let current = config.snapshot();
                    if current.session_token() != event.session_token {
                        continue;
                    }
                    presence_running = false;
                    if current.discord_presence() && event.presence.is_playing() {
                        discord.update(event.presence.as_ref(), &zelda.live_presence());
                    }
                    next_presence = presence_deadline(&mut presence_refresh_requested);
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

        if !auth_running && auth_pending && let Ok(Some(callback)) = take_callback() {
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
            let generation = account_generation.load(Ordering::Acquire);
            spawn_sync(
                Arc::clone(&sync_engine),
                Arc::clone(&stop),
                Arc::clone(&account_generation),
                sender.clone(),
                true,
                generation,
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

        #[cfg(target_os = "windows")]
        while let Ok(tray_event) = TrayIconEvent::receiver().try_recv() {
            if matches!(
                tray_event,
                TrayIconEvent::DoubleClick {
                    button: MouseButton::Left,
                    ..
                }
            ) {
                let folder = config.snapshot().destination_folder().to_owned();
                let _ = platform::open_folder(std::path::Path::new(&folder));
            }
        }

        while let Ok(menu_event) = MenuEvent::receiver().try_recv() {
            let snapshot = config.snapshot();
            if menu.matches_item(&menu_event, &menu.exit) {
                start_exit_watchdog();
                stop.store(true, Ordering::Release);
                unregister_protocol();
                let _ = clear_callback();
                zelda.stop_live_session();
                discord.clear();
                *control_flow = ControlFlow::Exit;
                return;
            }
            if menu.matches_item(&menu_event, &menu.sign_in) {
                if snapshot.session_token().is_empty() {
                    begin_sign_in(auth.as_ref(), &menu, &mut auth_pending);
                } else {
                    auth_pending = false;
                    unregister_protocol();
                    let _ = clear_callback();
                    initial_sync_deferred = false;
                    sync_running = false;
                    presence_running = false;
                    presence_refresh_requested = false;
                    clear_account_state(AccountStateRefs {
                        config: &config,
                        auth: auth.as_ref(),
                        nxapi: nxapi.as_ref(),
                        coral: coral.as_ref(),
                        splatnet: splatnet.as_ref(),
                        game_services: game_services.as_ref(),
                        zelda: zelda.as_ref(),
                        discord: discord.as_ref(),
                        enrichment_state: enrichment_state.as_ref(),
                        generation: account_generation.as_ref(),
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
            } else if menu.matches_item(&menu_event, &menu.sync_now) && !sync_running {
                if !snapshot.session_token().is_empty() {
                    initial_sync_deferred = false;
                    sync_running = true;
                    menu.set_status("Syncing album…");
                    let generation = account_generation.load(Ordering::Acquire);
                    spawn_sync(
                        Arc::clone(&sync_engine),
                        Arc::clone(&stop),
                        Arc::clone(&account_generation),
                        sender.clone(),
                        false,
                        generation,
                    );
                    if snapshot.discord_presence() {
                        if presence_running {
                            presence_refresh_requested = true;
                        } else {
                            next_presence = Instant::now();
                        }
                    }
                }
            } else if menu.matches_check(&menu_event, &menu.auto_sync) {
                let updated = config.update(AppConfig::toggle_auto_sync).ok();
                initial_sync_deferred = false;
                if let Some(updated) = updated {
                    menu.set_status(if updated.auto_sync() {
                        "Auto-sync enabled"
                    } else {
                        "Auto-sync disabled"
                    });
                    if updated.auto_sync()
                        && !updated.session_token().is_empty()
                        && !sync_running
                    {
                        sync_running = true;
                        let generation = account_generation.load(Ordering::Acquire);
                        spawn_sync(
                            Arc::clone(&sync_engine),
                            Arc::clone(&stop),
                            Arc::clone(&account_generation),
                            sender.clone(),
                            false,
                            generation,
                        );
                        next_auto_sync = jittered_deadline(sync_interval(&updated));
                    }
                    if updated.notifications() {
                        platform::notify(
                            "NSO Album Sync",
                            if updated.auto_sync() {
                                "Auto-sync enabled (refreshes every hour)."
                            } else {
                                "Auto-sync disabled."
                            },
                        );
                    }
                }
                menu.refresh(
                    &config.snapshot(),
                    platform::start_on_boot_enabled(),
                    current_status_for_refresh(&menu),
                );
            } else if menu.matches_check(&menu_event, &menu.notifications) {
                let _ = config.update(AppConfig::toggle_notifications);
                menu.refresh(
                    &config.snapshot(),
                    platform::start_on_boot_enabled(),
                    current_status_for_refresh(&menu),
                );
            } else if menu.matches_check(&menu_event, &menu.discord) {
                let updated = config.update(AppConfig::toggle_discord_presence).ok();
                if updated.as_ref().is_some_and(AppConfig::discord_presence) {
                    if presence_running {
                        presence_refresh_requested = true;
                    } else {
                        next_presence = Instant::now();
                    }
                    menu.set_status(
                        "Discord presence enabled — visibility is controlled by Discord Activity Sharing",
                    );
                    if updated.as_ref().is_some_and(AppConfig::notifications) {
                        platform::notify(
                            "Discord Rich Presence",
                            "Presence is enabled. In Discord, Activity Sharing must also be enabled for friends or server members to see it.",
                        );
                    }
                } else {
                    release_initial_sync(
                        &mut initial_sync_deferred,
                        &mut next_auto_sync,
                        &config,
                    );
                    presence_refresh_requested = false;
                    reset_presence_state(enrichment_state.as_ref());
                    zelda.stop_live_session();
                    discord.clear();
                    menu.set_status("Discord presence disabled");
                }
                menu.refresh(
                    &config.snapshot(),
                    platform::start_on_boot_enabled(),
                    current_status_for_refresh(&menu),
                );
            } else if menu.matches_check(&menu_event, &menu.start_boot) {
                let enabled = !platform::start_on_boot_enabled();
                match platform::set_start_on_boot(enabled) {
                    Ok(()) => {
                        let _ = config.update(|value| value.set_start_on_boot(enabled));
                    }
                    Err(error) => platform::show_error("Start on boot", &error.to_string()),
                }
                menu.refresh(
                    &config.snapshot(),
                    platform::start_on_boot_enabled(),
                    current_status_for_refresh(&menu),
                );
            } else if menu.matches_item(&menu_event, &menu.choose_folder) {
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
            } else if menu.matches_item(&menu_event, &menu.open_folder) {
                if let Err(error) =
                    platform::open_folder(std::path::Path::new(snapshot.destination_folder()))
                {
                    platform::show_error("Open album folder", &error.to_string());
                }
            } else if menu.matches_item(&menu_event, &menu.proxy_settings) {
                let current = config.snapshot();
                let proxy = platform::prompt(
                    "HTTP Proxy",
                    "Enter a proxy URL.",
                    current.proxy_url(),
                );
                let normalized = proxy.trim().to_owned();
                match config.update(|value| value.set_proxy_url(normalized.clone())) {
                    Ok(_) => {
                        shared_http.set_proxy(normalized.clone());
                        menu.refresh(
                            &config.snapshot(),
                            platform::start_on_boot_enabled(),
                            if normalized.is_empty() {
                                "HTTP proxy disabled"
                            } else {
                                "HTTP proxy updated"
                            },
                        );
                    }
                    Err(error) => platform::show_error("HTTP Proxy", &error.to_string()),
                }
            }
        }
    });
}

fn current_status_for_refresh(menu: &TrayMenu) -> &str {
    #[cfg(target_os = "macos")]
    {
        return menu.status_text.borrowed();
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = menu;
        "Ready"
    }
}

struct SyncEvent {
    result: anyhow::Result<SyncResult>,
    background: bool,
    generation: u64,
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

fn begin_sign_in(auth: &NintendoAuthManager, menu: &TrayMenu, auth_pending: &mut bool) {
    if *auth_pending {
        platform::notify(
            "Nintendo Sign-In",
            "A Nintendo Account sign-in is already waiting in your browser.",
        );
        return;
    }
    if !platform::confirm(NXAPI_DISCLOSURE_TITLE, NXAPI_DISCLOSURE) {
        menu.set_status("Sign-in cancelled");
        return;
    }

    let _ = clear_callback();
    if register_protocol().is_err() {
        menu.set_status(CALLBACK_REGISTRATION_ERROR);
        platform::notify("Nintendo Sign-In", CALLBACK_REGISTRATION_ERROR);
        return;
    }

    match auth.authorize_url() {
        Ok(url) => {
            *auth_pending = true;
            menu.set_status("Waiting for Nintendo Account sign-in in your browser…");
            if let Err(error) = open::that(url) {
                *auth_pending = false;
                unregister_protocol();
                let _ = clear_callback();
                menu.set_status(&error.to_string());
                platform::notify("Nintendo Sign-In", &error.to_string());
            }
        }
        Err(error) => {
            unregister_protocol();
            let _ = clear_callback();
            menu.set_status(&error.to_string());
            platform::notify("Nintendo Sign-In", &error.to_string());
        }
    }
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
    generation_counter: Arc<AtomicU64>,
    sender: mpsc::Sender<AppEvent>,
    background: bool,
    generation: u64,
) {
    std::thread::spawn(move || {
        let result = engine.sync(|| {
            stop.load(Ordering::Acquire)
                || generation_counter.load(Ordering::Acquire) != generation
        });
        let _ = sender.send(AppEvent::Sync(SyncEvent {
            result,
            background,
            generation,
        }));
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
                && let Ok(extra) = deps
                    .game_services
                    .fetch_animal_crossing_presence(&token)
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

fn presence_deadline(refresh_requested: &mut bool) -> Instant {
    if std::mem::take(refresh_requested) {
        Instant::now()
    } else {
        jittered_deadline(PRESENCE_POLL_INTERVAL)
    }
}

struct AccountStateRefs<'a> {
    config: &'a ConfigManager,
    auth: &'a NintendoAuthManager,
    nxapi: &'a NxapiClient,
    coral: &'a CoralClient,
    splatnet: &'a SplatNetClient,
    game_services: &'a GameServicesClient,
    zelda: &'a ZeldaNotesClient,
    discord: &'a DiscordPresence,
    enrichment_state: &'a Mutex<PresenceEnrichmentState>,
    generation: &'a AtomicU64,
}

fn clear_account_state(deps: AccountStateRefs<'_>) {
    let _ = deps.generation.fetch_add(1, Ordering::AcqRel);
    let _ = deps.config.clear_session();
    deps.auth.clear_cached_tokens();
    deps.nxapi.clear_user_auth();
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

fn start_exit_watchdog() {
    std::thread::spawn(|| {
        std::thread::sleep(EXIT_WATCHDOG_DELAY);
        std::process::exit(0);
    });
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

struct TrayMenu {
    root: Menu,
    nickname: MenuItem,
    last_sync: MenuItem,
    sync_now: MenuItem,
    auto_sync: CheckMenuItem,
    notifications: CheckMenuItem,
    discord: CheckMenuItem,
    choose_folder: MenuItem,
    open_folder: MenuItem,
    start_boot: CheckMenuItem,
    proxy_settings: MenuItem,
    sign_in: MenuItem,
    exit: MenuItem,
    _separators: [PredefinedMenuItem; 4],
    #[cfg(target_os = "macos")]
    status: MenuItem,
    #[cfg(target_os = "macos")]
    status_attached: Cell<bool>,
    #[cfg(target_os = "macos")]
    status_text: StatusText,
}

#[cfg(target_os = "macos")]
struct StatusText(std::cell::RefCell<String>);

#[cfg(target_os = "macos")]
impl StatusText {
    fn new() -> Self {
        Self(std::cell::RefCell::new("Ready".to_owned()))
    }

    fn set(&self, value: &str) {
        *self.0.borrow_mut() = value.to_owned();
    }

    fn borrowed(&self) -> &str {
        "Ready"
    }
}

impl TrayMenu {
    fn new(config: &AppConfig, start_on_boot: bool) -> anyhow::Result<Self> {
        let root = Menu::new();
        let nickname = MenuItem::with_id("nickname", "", false, None);
        let last_sync = MenuItem::with_id("last_sync", "", false, None);
        let sync_now = MenuItem::with_id("sync_now", "Sync Now", true, None);
        let auto_sync = CheckMenuItem::with_id(
            "auto_sync",
            auto_sync_label(config.sync_interval_minutes()),
            true,
            config.auto_sync(),
            None,
        );
        let notifications = CheckMenuItem::with_id(
            "notifications",
            "Notifications",
            true,
            config.notifications(),
            None,
        );
        let discord = CheckMenuItem::with_id(
            "discord",
            "Discord Rich Presence",
            true,
            config.discord_presence(),
            None,
        );
        let choose_folder =
            MenuItem::with_id("choose_folder", "Choose Album Folder…", true, None);
        let open_folder = MenuItem::with_id("open_folder", "Open Album Folder", true, None);
        let start_boot =
            CheckMenuItem::with_id("start_boot", "Start on Boot", true, start_on_boot, None);
        let proxy_settings = MenuItem::with_id("proxy_settings", "HTTP Proxy…", true, None);
        let sign_in = MenuItem::with_id("sign_in", "", true, None);
        let exit = MenuItem::with_id("exit", "Exit", true, None);
        let separators = [
            PredefinedMenuItem::separator(),
            PredefinedMenuItem::separator(),
            PredefinedMenuItem::separator(),
            PredefinedMenuItem::separator(),
        ];
        #[cfg(target_os = "macos")]
        let status = MenuItem::with_id("status", "", false, None);

        root.append(&nickname)?;
        root.append(&last_sync)?;
        root.append(&separators[0])?;
        root.append(&sync_now)?;
        root.append(&auto_sync)?;
        root.append(&notifications)?;
        root.append(&discord)?;
        root.append(&separators[1])?;
        root.append(&choose_folder)?;
        root.append(&open_folder)?;
        root.append(&separators[2])?;
        root.append(&start_boot)?;
        root.append(&proxy_settings)?;
        root.append(&sign_in)?;
        root.append(&separators[3])?;
        root.append(&exit)?;

        let menu = Self {
            root,
            nickname,
            last_sync,
            sync_now,
            auto_sync,
            notifications,
            discord,
            choose_folder,
            open_folder,
            start_boot,
            proxy_settings,
            sign_in,
            exit,
            _separators: separators,
            #[cfg(target_os = "macos")]
            status,
            #[cfg(target_os = "macos")]
            status_attached: Cell::new(false),
            #[cfg(target_os = "macos")]
            status_text: StatusText::new(),
        };
        menu.refresh(config, start_on_boot, "Ready");
        Ok(menu)
    }

    fn refresh(&self, config: &AppConfig, start_on_boot: bool, status: &str) {
        self.nickname.set_text(if config.session_token().is_empty() {
            "Not signed in".to_owned()
        } else {
            format!("Connected as {}", config.user_nickname())
        });
        self.last_sync.set_text(format!(
            "Last sync: {}",
            if config.last_sync().is_empty() {
                "Never"
            } else {
                config.last_sync()
            }
        ));
        self.sync_now
            .set_enabled(!config.session_token().is_empty());
        self.auto_sync
            .set_text(auto_sync_label(config.sync_interval_minutes()));
        self.auto_sync.set_checked(config.auto_sync());
        self.notifications.set_checked(config.notifications());
        self.discord.set_checked(config.discord_presence());
        self.start_boot.set_checked(start_on_boot);
        self.sign_in.set_text(if config.session_token().is_empty() {
            "Sign In to Nintendo Account…"
        } else {
            "Sign Out"
        });
        self.set_status(status);
    }

    fn set_status(&self, status: &str) {
        #[cfg(target_os = "macos")]
        {
            self.status_text.set(status);
            let visible = !status.is_empty() && status != "Ready";
            if visible {
                self.status.set_text(status);
                if !self.status_attached.get() && self.root.insert(&self.status, 1).is_ok() {
                    self.status_attached.set(true);
                }
            } else if self.status_attached.get() && self.root.remove(&self.status).is_ok() {
                self.status_attached.set(false);
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            let _ = status;
        }
    }

    fn matches_item(&self, event: &MenuEvent, item: &MenuItem) -> bool {
        event.id == item.id()
    }

    fn matches_check(&self, event: &MenuEvent, item: &CheckMenuItem) -> bool {
        event.id == item.id()
    }
}

fn auto_sync_label(minutes: u32) -> String {
    let safe_minutes = minutes.max(1);
    if safe_minutes == 60 {
        "Auto-Sync (Hourly)".to_owned()
    } else {
        format!("Auto-Sync (Every {safe_minutes} min)")
    }
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
    let directory = ico::IconDir::read(Cursor::new(include_bytes!("../app.ico").as_slice()))?;
    let entry = directory
        .entries()
        .iter()
        .min_by_key(|entry| {
            let exact = u8::from(entry.width() != 32 || entry.height() != 32);
            let distance = entry.width().abs_diff(32) + entry.height().abs_diff(32);
            (exact, distance)
        })
        .ok_or_else(|| anyhow::anyhow!("app.ico contains no icon images"))?;
    let image = entry.decode()?;
    Ok(Icon::from_rgba(
        image.rgba_data().to_vec(),
        image.width(),
        image.height(),
    )?)
}
