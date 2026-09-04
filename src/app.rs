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
use crate::zelda_notes::{ZELDA_NOTES_GAME_SERVICE_ID, ZELDA_NOTES_GAME_SERVICE_ID_ALT, ZeldaNotesClient, game_for_presence};
use crate::zelda_regions::ZeldaGame;
use rand::Rng as _;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};
use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoop};
use tray_icon::menu::{Menu, MenuEvent, MenuItem};
use tray_icon::{Icon, TrayIconBuilder};

const PRESENCE_POLL_INTERVAL: Duration = Duration::from_secs(60);

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
    let discord = DiscordPresence::new(initial.discord_application_id());

    let (sender, receiver) = mpsc::channel::<AppEvent>();
    let stop = Arc::new(AtomicBool::new(false));
    let mut sync_running = false;
    let mut auth_running = false;
    let mut presence_running = false;
    let mut next_auto_sync = Instant::now();
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
                AppEvent::SyncFinished(result) => {
                    sync_running = false;
                    match result {
                        Ok(result) => {
                            let now = chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
                            let _ = config.update(|value| value.set_last_sync(now));
                            let current = config.snapshot();
                            menu.refresh(&current, platform::start_on_boot_enabled(), "Ready");
                            if current.notifications() && result.new_downloads() > 0 {
                                platform::notify(
                                    "NSO Album Sync",
                                    &format!("Downloaded {} new album item(s).", result.new_downloads()),
                                );
                            }
                        }
                        Err(error) => {
                            menu.set_status(&format!("Sync failed: {error}"));
                            if config.snapshot().notifications() {
                                platform::show_error("NSO Album Sync", &error.to_string());
                            }
                        }
                    }
                }
                AppEvent::AuthFinished(result) => {
                    auth_running = false;
                    match result {
                        Ok((token, nickname)) => {
                            let _ = config.update(|value| value.set_session(token, nickname));
                            coral.clear_cached_session();
                            splatnet.clear_cache();
                            game_services.clear_cache();
                            zelda.clear_cache();
                            menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Signed in");
                            next_presence = Instant::now();
                            next_auto_sync = Instant::now();
                        }
                        Err(error) => {
                            menu.set_status("Sign in failed");
                            platform::show_error("Nintendo Account sign in", &error.to_string());
                        }
                    }
                }
                AppEvent::PresenceFinished(result) => {
                    presence_running = false;
                    match result {
                        Ok(presence) => {
                            let current = config.snapshot();
                            if current.discord_presence() {
                                discord.update(&presence, &zelda.live_presence());
                            } else {
                                discord.clear();
                            }
                            menu.set_status(if presence.is_playing() { presence.game_name() } else { "Ready" });
                        }
                        Err(error) => {
                            discord.clear();
                            menu.set_status(&format!("Presence error: {error}"));
                        }
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

        if !auth_running {
            if let Ok(Some(callback)) = take_callback() {
                auth_running = true;
                menu.set_status("Completing Nintendo sign in…");
                spawn_auth_completion(Arc::clone(&auth), sender.clone(), callback);
            }
        }

        let snapshot = config.snapshot();
        if snapshot.auto_sync()
            && !snapshot.session_token().is_empty()
            && !sync_running
            && Instant::now() >= next_auto_sync
        {
            sync_running = true;
            menu.set_status("Syncing album…");
            spawn_sync(Arc::clone(&sync_engine), Arc::clone(&stop), sender.clone());
            next_auto_sync = jittered_deadline(Duration::from_secs(
                u64::from(snapshot.sync_interval_minutes()) * 60,
            ));
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
                    match auth.authorize_url() {
                        Ok(url) => {
                            menu.set_status("Waiting for Nintendo sign in…");
                            if let Err(error) = open::that(url) {
                                platform::show_error("Nintendo Account sign in", &error.to_string());
                            }
                        }
                        Err(error) => platform::show_error("Nintendo Account sign in", &error.to_string()),
                    }
                } else {
                    let _ = config.clear_session();
                    auth.clear_cached_tokens();
                    coral.clear_cached_session();
                    splatnet.clear_cache();
                    game_services.clear_cache();
                    zelda.clear_cache();
                    discord.clear();
                    menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Signed out");
                }
            } else if menu.matches(&menu_event, &menu.sync_now) && !sync_running {
                if snapshot.session_token().is_empty() {
                    platform::show_info("NSO Album Sync", "Sign in to your Nintendo Account first.");
                } else {
                    sync_running = true;
                    menu.set_status("Syncing album…");
                    spawn_sync(Arc::clone(&sync_engine), Arc::clone(&stop), sender.clone());
                }
            } else if menu.matches(&menu_event, &menu.auto_sync) {
                let _ = config.update(AppConfig::toggle_auto_sync);
                next_auto_sync = Instant::now();
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.notifications) {
                let _ = config.update(AppConfig::toggle_notifications);
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            } else if menu.matches(&menu_event, &menu.discord) {
                let updated = config.update(AppConfig::toggle_discord_presence).ok();
                if updated.as_ref().is_some_and(AppConfig::discord_presence) {
                    next_presence = Instant::now();
                } else {
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
                    menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
                }
            } else if menu.matches(&menu_event, &menu.open_folder) {
                if let Err(error) = platform::open_folder(std::path::Path::new(snapshot.destination_folder())) {
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
            } else if let Some(minutes) = menu.interval_for(&menu_event) {
                let _ = config.update(|value| value.set_sync_interval_minutes(minutes));
                next_auto_sync = jittered_deadline(Duration::from_secs(u64::from(minutes) * 60));
                menu.refresh(&config.snapshot(), platform::start_on_boot_enabled(), "Ready");
            }
        }
    });
}

enum AppEvent {
    SyncFinished(anyhow::Result<SyncResult>),
    AuthFinished(anyhow::Result<(String, String)>),
    PresenceFinished(anyhow::Result<NintendoPresence>),
}

fn spawn_auth_completion(
    auth: Arc<NintendoAuthManager>,
    sender: mpsc::Sender<AppEvent>,
    callback: String,
) {
    std::thread::spawn(move || {
        let result = auth
            .complete_login(&callback)
            .map(|auth_result| {
                (
                    auth_result.session_token().to_owned(),
                    auth_result.user_nickname().to_owned(),
                )
            });
        let _ = sender.send(AppEvent::AuthFinished(result));
    });
}

fn spawn_sync(engine: Arc<SyncEngine>, stop: Arc<AtomicBool>, sender: mpsc::Sender<AppEvent>) {
    std::thread::spawn(move || {
        let result = engine.sync(|| stop.load(Ordering::Acquire));
        let _ = sender.send(AppEvent::SyncFinished(result));
    });
}

struct PresenceDependencies {
    auth: Arc<NintendoAuthManager>,
    coral: Arc<CoralClient>,
    splatnet: Arc<SplatNetClient>,
    game_services: Arc<GameServicesClient>,
    zelda: Arc<ZeldaNotesClient>,
}

fn spawn_presence(
    deps: PresenceDependencies,
    sender: mpsc::Sender<AppEvent>,
    session_token: String,
) {
    std::thread::spawn(move || {
        let result = fetch_enriched_presence(&deps, &session_token);
        let _ = sender.send(AppEvent::PresenceFinished(result));
    });
}

fn fetch_enriched_presence(
    deps: &PresenceDependencies,
    session_token: &str,
) -> anyhow::Result<NintendoPresence> {
    let mut presence = deps.coral.self_presence(session_token)?;
    let tokens = deps.auth.exchange_session_token(session_token)?;
    let profile = deps.auth.fetch_profile(tokens.access_token())?;
    deps.splatnet.set_locale(profile.language(), profile.country());
    deps.game_services.set_locale(profile.language(), profile.country());
    deps.zelda.set_locale(profile.language(), profile.country());

    if !presence.is_playing() {
        deps.zelda.set_active_game(ZeldaGame::Unknown);
        return Ok(presence);
    }
    if is_splatoon3(&presence) {
        if let Ok(token) = service_token_with_fallback(
            &deps.coral,
            session_token,
            SPLATNET3_GAME_SERVICE_ID,
            SPLATNET3_GAME_SERVICE_ID_ALT,
        ) {
            if let Ok(extra) = deps.splatnet.fetch_presence(&token) {
                if extra.active() {
                    presence.set_custom_details(extra.format_details());
                    presence.set_custom_state(extra.format_state());
                    if !extra.stage_image_uri().is_empty() {
                        presence.set_custom_image_uri(extra.stage_image_uri().to_owned());
                    }
                }
            }
        }
    } else if is_animal_crossing(&presence) {
        if let Ok(token) = deps
            .coral
            .get_web_service_token(session_token, ANIMAL_CROSSING_GAME_SERVICE_ID)
        {
            if let Ok(extra) = deps.game_services.fetch_animal_crossing_presence(&token) {
                if extra.active() {
                    presence.set_custom_details(extra.format_details());
                    presence.set_custom_state(extra.format_state());
                    if !extra.image_uri().is_empty() {
                        presence.set_custom_image_uri(extra.image_uri().to_owned());
                    }
                }
            }
        }
    } else if is_splatoon2(&presence) {
        if let Ok(token) = deps
            .coral
            .get_web_service_token(session_token, SPLATOON2_GAME_SERVICE_ID)
        {
            if let Ok(extra) = deps.game_services.fetch_splatoon2_presence(&token) {
                if extra.active() {
                    presence.set_custom_details(extra.format_details());
                    presence.set_custom_state(extra.format_state());
                    if !extra.stage_image_uri().is_empty() {
                        presence.set_custom_image_uri(extra.stage_image_uri().to_owned());
                    }
                }
            }
        }
    }

    let zelda_game = game_for_presence(presence.title_id(), presence.game_name());
    if zelda_game != ZeldaGame::Unknown {
        if let Ok(token) = service_token_with_fallback(
            &deps.coral,
            session_token,
            ZELDA_NOTES_GAME_SERVICE_ID,
            ZELDA_NOTES_GAME_SERVICE_ID_ALT,
        ) {
            let _ = deps.zelda.fetch_presence(&token);
            deps.zelda.set_active_game(zelda_game);
        }
    } else {
        deps.zelda.set_active_game(ZeldaGame::Unknown);
    }
    Ok(presence)
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

fn is_splatoon3(presence: &NintendoPresence) -> bool {
    presence.title_id() == "0100c2500fc20000"
        || presence.game_name().contains("Splatoon 3")
        || presence.game_name().contains("スプラトゥーン3")
}

fn is_splatoon2(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01003bc0000a0000"
        || presence.game_name().contains("Splatoon 2")
        || presence.game_name().contains("スプラトゥーン2")
}

fn is_animal_crossing(presence: &NintendoPresence) -> bool {
    presence.title_id() == "01006f8002326000"
        || presence.game_name().contains("Animal Crossing")
        || presence.game_name().contains("あつまれ どうぶつの森")
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
            &menu.exit,
        ] {
            root.append(item)?;
        }
        menu.refresh(config, start_on_boot, "Ready");
        Ok(menu)
    }

    fn refresh(&self, config: &AppConfig, start_on_boot: bool, status: &str) {
        self.nickname.set_text(format!("User: {}", config.user_nickname()));
        self.last_sync.set_text(format!("Last sync: {}", config.last_sync()));
        self.status.set_text(format!("Status: {status}"));
        self.sign_in.set_text(if config.session_token().is_empty() {
            "Sign in to Nintendo Account…"
        } else {
            "Sign out of Nintendo Account"
        });
        self.auto_sync.set_text(toggle_text("Auto sync", config.auto_sync()));
        self.notifications
            .set_text(toggle_text("Notifications", config.notifications()));
        self.discord
            .set_text(toggle_text("Discord Rich Presence", config.discord_presence()));
        self.start_boot.set_text(toggle_text("Start on boot", start_on_boot));
        self.sync_now.set_enabled(!config.session_token().is_empty());
    }

    fn set_status(&self, status: &str) {
        self.status.set_text(format!("Status: {status}"));
    }

    fn matches(&self, event: &MenuEvent, item: &MenuItem) -> bool {
        &event.id == item.id()
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
