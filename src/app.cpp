#include "nso_album_sync/app.hpp"

#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <random>
#include <thread>

namespace nso {
namespace {

constexpr auto kPresencePollInterval = std::chrono::seconds(60);
constexpr auto kAuthCallbackPollInterval = std::chrono::milliseconds(150);
constexpr auto kExitWatchdogDelay = std::chrono::seconds(5);
constexpr std::int64_t kPollingJitterDivisor = 50;  // +/- 2%

void log_app_presence(const std::string& msg) {
    std::cerr << "[AppPresence] " << msg << "\n";
    try {
        const auto path = std::filesystem::temp_directory_path() / "nso-album-sync-rpc.log";
        std::ofstream output(path, std::ios::app);
        if (output) output << "[AppPresence] " << msg << '\n';
    } catch (...) {}
}

constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiDisclosure[] =
    "NSO Album Sync uses the third-party nxapi-znca-api service at "
    "fancy.org.uk for Nintendo Switch Online request attestation and "
    "request/response encryption.\n\n"
    "When Nintendo Switch Online features are used, your Nintendo Account "
    "id_token and the profile fields required by Coral (Nintendo Account ID, "
    "birthday, country, and language), your Coral access token, and the Coral "
    "API requests and responses used by this app are sent to and processed by "
    "that third-party service. These tokens can authenticate Nintendo services "
    "while they remain valid.\n\n"
    "Continue with Nintendo Account sign-in?";

std::chrono::milliseconds jittered_interval(std::chrono::milliseconds nominal) {
    const auto nominal_ms = std::max<std::int64_t>(1, nominal.count());
    const auto jitter_ms = std::max<std::int64_t>(1, nominal_ms / kPollingJitterDivisor);

    static thread_local std::mt19937_64 generator([] {
        std::random_device random;
        std::seed_seq seed{
            random(), random(), random(), random(),
            random(), random(), random(), random(),
        };
        return std::mt19937_64(seed);
    }());

    std::uniform_int_distribution<std::int64_t> distribution(-jitter_ms, jitter_ms);
    return std::chrono::milliseconds(
        std::max<std::int64_t>(1, nominal_ms + distribution(generator)));
}

std::string current_time_text() {
    const auto now = std::time(nullptr);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char buffer[48];
    std::strftime(buffer, sizeof(buffer), "%H:%M (%Y-%m-%d)", &local_time);
    return buffer;
}

bool is_invalid_grant(const std::string& message) {
    return message.find("invalid_grant") != std::string::npos;
}

enum class RpcGameService {
    None,
    Splatoon3,
    ZeldaNotes,
    AnimalCrossing,
    Splatoon2,
};

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const auto* needle : needles) {
        if (needle != nullptr && text.find(needle) != std::string::npos) return true;
    }
    return false;
}

RpcGameService rpc_game_service_for(const NintendoPresence& presence) {
    // Prefer Nintendo's stable application/title ID. Coral names are localized,
    // so routing only by English/Japanese substrings silently disables RPC
    // enrichment for other Nintendo Account languages.
    if (presence.title_id == "0100c2500fc20000") return RpcGameService::Splatoon3;
    if (presence.title_id == "01003bc0000a0000") return RpcGameService::Splatoon2;
    if (presence.title_id == "01006f8002326000") return RpcGameService::AnimalCrossing;
    if (presence.title_id == "01007ef00011e000" ||
        presence.title_id == "0100f2c0115b6000") {
        return RpcGameService::ZeldaNotes;
    }

    // Name fallbacks cover Coral payloads that omit an application ID and
    // Nintendo Switch 2 Edition display names. Keep Zelda deliberately narrow:
    // Zelda Notes supports BOTW/TOTK, not every Zelda-family title.
    if (contains_any(presence.game_name, {"Splatoon 3", "スプラトゥーン3"})) {
        return RpcGameService::Splatoon3;
    }
    if (contains_any(presence.game_name, {
            "Breath of the Wild", "ブレス オブ ザ ワイルド",
            "Tears of the Kingdom", "ティアーズ オブ ザ キングダム"})) {
        return RpcGameService::ZeldaNotes;
    }
    if (contains_any(presence.game_name, {
            "Animal Crossing", "New Horizons", "どうぶつの森", "あつ森"})) {
        return RpcGameService::AnimalCrossing;
    }
    if (contains_any(presence.game_name, {"Splatoon 2", "スプラトゥーン2"})) {
        return RpcGameService::Splatoon2;
    }
    return RpcGameService::None;
}

}  // namespace

App::App()
    : auth_(http_),
      nxapi_(
          http_,
          config_.snapshot().nxapi_auth_client_id,
          config_.directory() / "nxapi-cache.json"),
      coral_(http_, auth_, nxapi_, config_.directory()),
      splatnet_(http_),
      zeldanotes_(http_),
      game_services_(http_),
      sync_(config_, coral_, http_),
      discord_(config_.snapshot().discord_application_id) {
    const auto config = config_.snapshot();
    http_.set_proxy(config.proxy_url);
    last_sync_ = config.last_sync.empty() ? "Never" : config.last_sync;
}

App::~App() {
    stop_workers();
}

void App::wake_workers() {
    sleep_cv_.notify_all();
    presence_cv_.notify_all();
    sync_queue_cv_.notify_all();
}

void App::update_menu() {
    const auto config = config_.snapshot();
    MenuState menu;
    menu.nickname = config.user_nickname;
    {
        std::lock_guard lock(state_mutex_);
        menu.last_sync = last_sync_;
        menu.status = status_;
    }
    menu.auto_sync = config.auto_sync;
    menu.notifications = config.notifications;
    menu.discord = config.discord_presence;
    menu.start_on_boot = start_on_boot_enabled();
    menu.signed_in = !config.session_token.empty();
    menu.sync_interval_minutes = std::max(1, config.sync_interval_minutes);
    ui_.update(menu);
}

void App::invalidate_session(const std::string& reason) {
    account_generation_.fetch_add(1);
    coral_.clear_cached_session();
    splatnet_.clear_cache();
    zeldanotes_.clear_cache();
    game_services_.clear_cache();
    nxapi_.clear_user_auth();
    auth_.clear_cached_tokens();
    config_.clear_session();
    config_.update([](AppConfig& config) {
        config.user_nickname.clear();
    });
    discord_.clear();
    {
        std::lock_guard state_lock(state_mutex_);
        status_ = reason;
    }
    update_menu();
    wake_workers();
}

void App::sync_now(bool background) {
    const auto generation = account_generation_.load();
    try {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Syncing album…";
        }
        update_menu();

        const auto result = sync_.sync([this, generation] {
            return stopping_.load() || account_generation_.load() != generation;
        });
        if (stopping_ || account_generation_.load() != generation) return;

        const auto sync_time = current_time_text();
        {
            std::lock_guard state_lock(state_mutex_);
            last_sync_ = sync_time;
            status_ = "Ready";
        }
        const auto config = config_.update([&](AppConfig& value) {
            value.last_sync = sync_time;
        });

        if (config.notifications) {
            if (result.new_downloads > 0) {
                ui_.notify(
                    "NSO Album Sync",
                    "Synced " + std::to_string(result.new_downloads) +
                        (result.new_downloads == 1
                             ? " new capture to your album folder!"
                             : " new captures to your album folder!"));
            } else if (!background) {
                ui_.notify(
                    "NSO Album Sync",
                    "Album is up to date. No new captures found.");
            }
        }
    } catch (const std::exception& error) {
        if (stopping_ || account_generation_.load() != generation) return;
        if (is_invalid_grant(error.what())) {
            invalidate_session(
                "Nintendo Account session expired. Sign in again to continue.");
            return;
        }
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = error.what();
        }
        if (config_.snapshot().notifications) {
            ui_.notify("NSO Album Sync", error.what());
        }
    }
    update_menu();
}

void App::queue_sync(bool background) {
    std::lock_guard lock(sync_queue_mutex_);
    if (stopping_) return;
    if (!sync_requested_) {
        sync_requested_ = true;
        sync_request_background_ = background;
    } else if (!background) {
        // A manual request should keep its user-visible completion notification
        // even if an automatic request was already queued.
        sync_request_background_ = false;
    }
    sync_queue_cv_.notify_one();
}

void App::request_presence_refresh() {
    if (stopping_) return;
    presence_refresh_requested_.store(true, std::memory_order_release);
    presence_cv_.notify_one();
}

void App::sync_loop() {
    for (;;) {
        bool background = true;
        {
            std::unique_lock lock(sync_queue_mutex_);
            sync_queue_cv_.wait(lock, [this] {
                return stopping_.load() || sync_requested_;
            });
            if (stopping_) break;
            background = sync_request_background_;
            sync_requested_ = false;
            sync_request_background_ = true;
        }
        sync_now(background);
    }
}

void App::complete_pending_login(const std::string& redirect_url_or_code) {
    std::lock_guard auth_lock(auth_flow_mutex_);
    if (!auth_pending_) return;

    try {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Completing Nintendo Account sign-in…";
        }
        update_menu();

        const auto auth_result = auth_.complete_login(redirect_url_or_code);
        account_generation_.fetch_add(1);
        coral_.clear_cached_session();
        splatnet_.clear_cache();
        zeldanotes_.clear_cache();
        game_services_.clear_cache();
        nxapi_.clear_user_auth();
        const auto config = config_.update([&](AppConfig& value) {
            value.session_token = auth_result.session_token;
            value.user_nickname = auth_result.user_nickname;
        });

        auth_pending_ = false;
        unregister_nintendo_auth_protocol();
        clear_nintendo_auth_callback();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Connected as " + config.user_nickname;
        }
        update_menu();
        start_operational_workers();
        wake_workers();
        if (config.discord_presence) {
            request_presence_refresh();
        }
    } catch (const std::exception& error) {
        const std::string message = error.what();
        const bool wrong_state =
            message.find("invalid OAuth state") != std::string::npos;
        if (!wrong_state) {
            // A session-token code is one-time material. Once any real login
            // attempt fails, discard this flow so the next click starts with a
            // fresh PKCE verifier/code instead of repeatedly reusing a dead one.
            auth_pending_ = false;
            unregister_nintendo_auth_protocol();
            clear_nintendo_auth_callback();
        }
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = message;
        }
        update_menu();
        if (config_.snapshot().notifications) {
            ui_.notify("Nintendo Sign-In", message);
        }
    }
}

void App::sign_in_or_out() {
    const auto current = config_.snapshot();

    if (!current.session_token.empty()) {
        account_generation_.fetch_add(1);
        coral_.clear_cached_session();
        splatnet_.clear_cache();
        zeldanotes_.clear_cache();
        game_services_.clear_cache();
        nxapi_.clear_user_auth();
        auth_.clear_cached_tokens();
        config_.clear_session();
        config_.update([](AppConfig& config) {
            config.user_nickname.clear();
        });
        discord_.clear();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Signed out";
        }
        update_menu();
        wake_workers();
        if (current.notifications) {
            ui_.notify(
                "NSO Album Sync",
                "Signed out of your Nintendo Account.");
        }
        return;
    }

    if (auth_pending_) {
        ui_.notify(
            "Nintendo Sign-In",
            "A Nintendo Account sign-in is already waiting in your browser.");
        return;
    }

    if (!ui_.confirm(kNxapiDisclosureTitle, kNxapiDisclosure)) {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Sign-in cancelled";
        }
        update_menu();
        return;
    }

    clear_nintendo_auth_callback();
    if (!register_nintendo_auth_protocol()) {
        const std::string message =
            "Automatic Nintendo Account browser return could not be registered. "
            "Close any other app using the Nintendo Switch App sign-in link and try again.";
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = message;
        }
        update_menu();
        ui_.notify("Nintendo Sign-In", message);
        return;
    }

    try {
        const auto authorize_url = auth_.authorize_url();
        auth_pending_ = true;
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Waiting for Nintendo Account sign-in in your browser…";
        }
        update_menu();
        open_url(authorize_url);
    } catch (const std::exception& error) {
        auth_pending_ = false;
        unregister_nintendo_auth_protocol();
        clear_nintendo_auth_callback();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = error.what();
        }
        update_menu();
        ui_.notify("Nintendo Sign-In", error.what());
    }
}

void App::presence_loop() {
    std::string active_game_key;
    bool enrichment_attempted = false;
    std::string cached_custom_details;
    std::string cached_custom_state;
    std::string cached_custom_image_uri;

    const auto reset_enrichment = [&] {
        active_game_key.clear();
        enrichment_attempted = false;
        cached_custom_details.clear();
        cached_custom_state.clear();
        cached_custom_image_uri.clear();
    };

    while (!stopping_) {
        auto config = config_.snapshot();
        if (!config.discord_presence || config.session_token.empty()) {
            reset_enrichment();
            discord_.clear();
            std::unique_lock wait_lock(presence_sleep_mutex_);
            presence_cv_.wait(wait_lock, [this] {
                if (stopping_) return true;
                const auto state = config_.snapshot();
                return state.discord_presence && !state.session_token.empty();
            });
            continue;
        }

        // Reaching this point means one of three things made a presence poll
        // eligible: Rich Presence became active, the recurring timer elapsed,
        // or an explicit refresh was queued by Sync Now. Consume any queued
        // refresh before polling; a new request arriving during the network
        // call remains set and causes another immediate pass afterwards.
        presence_refresh_requested_.store(false, std::memory_order_release);

        const auto generation = account_generation_.load();
        const auto session_token = config.session_token;
        try {
            auto presence = coral_.self_presence(session_token);
            config = config_.snapshot();
            if (!stopping_ && config.discord_presence &&
                config.session_token == session_token &&
                account_generation_.load() == generation) {
                log_app_presence("Polled presence: is_playing=" + std::string(presence.is_playing() ? "true" : "false") + " title_id=" + presence.title_id + " game_name=" + presence.game_name);
                if (presence.is_playing()) {
                    const auto service = rpc_game_service_for(presence);
                    log_app_presence("Service routing: service=" + std::to_string(static_cast<int>(service)));
                    const auto game_key = !presence.title_id.empty()
                        ? presence.title_id
                        : presence.game_name;

                    // Treat a title transition as the start of a new play
                    // session. Rich game-service data is fetched at most once
                    // for that continuous session; later polls use Coral only
                    // and reapply the cached Discord fields.
                    if (active_game_key != game_key) {
                        active_game_key = game_key;
                        enrichment_attempted = false;
                        cached_custom_details.clear();
                        cached_custom_state.clear();
                        cached_custom_image_uri.clear();
                    }

                    const bool should_probe_game_service =
                        service != RpcGameService::None && !enrichment_attempted;

                    if (should_probe_game_service) {
                        // Mark the attempt before any network work. A transient
                        // game-service failure must not turn into automatic
                        // retries every minute; the next attempt is the next
                        // detected game session.
                        enrichment_attempted = true;

                        // Game WebView bootstraps use the Nintendo Account locale
                        // in nxapi and in the working backend. This profile data
                        // is only needed for the one enrichment probe.
                        try {
                            const auto tokens = auth_.exchange_session_token(session_token);
                            const auto profile = auth_.fetch_profile(tokens.access_token);
                            splatnet_.set_locale(profile.language, profile.country);
                            zeldanotes_.set_locale(profile.language, profile.country);
                            game_services_.set_locale(profile.language, profile.country);
                        } catch (...) {
                            // The service clients retain safe en-GB/GB defaults if
                            // the already-authenticated profile cannot be refreshed.
                        }

                        // Signing out/account switching can happen while the
                        // profile or Nintendo service request is in flight.
                        if (!stopping_ && account_generation_.load() == generation &&
                            config_.snapshot().session_token == session_token) {
                            switch (service) {
                                case RpcGameService::Splatoon3:
                                    try {
                                        const auto web_token = coral_.get_web_service_token(
                                            session_token, kSplatoon3GameServiceId);
                                        if (!web_token.empty()) {
                                            const auto splat_presence =
                                                splatnet_.fetch_presence(web_token);
                                            if (splat_presence.active) {
                                                presence.custom_details =
                                                    splat_presence.format_details();
                                                presence.custom_state =
                                                    splat_presence.format_state();
                                                if (!splat_presence.stage_image_uri.empty()) {
                                                    presence.custom_image_uri =
                                                        splat_presence.stage_image_uri;
                                                }
                                            }
                                        }
                                    } catch (...) {
                                    }
                                    break;
                                case RpcGameService::ZeldaNotes:
                                    try {
                                        auto web_token = coral_.get_web_service_token(
                                            session_token, kZeldaNotesGameServiceId);
                                        if (web_token.empty()) {
                                            std::cerr << "Presence: ZeldaNotes primary service token empty, falling back." << std::endl;
                                            web_token = coral_.get_web_service_token(
                                                session_token, kZeldaNotesGameServiceIdAlt);
                                        }
                                        if (!web_token.empty()) {
                                            const auto zelda_presence =
                                                zeldanotes_.fetch_presence(web_token);
                                            if (zelda_presence.active) {
                                                const auto state_str =
                                                    zelda_presence.format_state();
                                                const auto details_str =
                                                    zelda_presence.format_details();
                                                if (!state_str.empty()) {
                                                    presence.custom_state = state_str;
                                                }
                                                if (!details_str.empty()) {
                                                    presence.custom_details = details_str;
                                                }
                                                if (!zelda_presence.stage_image_uri.empty()) {
                                                    presence.custom_image_uri =
                                                        zelda_presence.stage_image_uri;
                                                }
                                            }
                                        }
                                    } catch (...) {
                                    }
                                    break;
                                case RpcGameService::AnimalCrossing:
                                    try {
                                        const auto web_token = coral_.get_web_service_token(
                                            session_token, kAnimalCrossingGameServiceId);
                                        if (!web_token.empty()) {
                                            const auto ac_presence =
                                                game_services_.fetch_animal_crossing_presence(web_token);
                                            if (ac_presence.active) {
                                                const auto state_str = ac_presence.format_state();
                                                const auto details_str = ac_presence.format_details();
                                                if (!state_str.empty()) presence.custom_state = state_str;
                                                if (!details_str.empty()) presence.custom_details = details_str;
                                                if (!ac_presence.image_uri.empty() &&
                                                    ac_presence.image_uri.size() <= 300) {
                                                    presence.custom_image_uri = ac_presence.image_uri;
                                                }
                                            }
                                        }
                                    } catch (...) {
                                    }
                                    break;

                                case RpcGameService::Splatoon2:
                                    try {
                                        const auto web_token = coral_.get_web_service_token(
                                            session_token, kSplatoon2GameServiceId);
                                        if (!web_token.empty()) {
                                            const auto splat2_presence =
                                                game_services_.fetch_splatoon2_presence(web_token);
                                            if (splat2_presence.active) {
                                                const auto state_str = splat2_presence.format_state();
                                                const auto details_str = splat2_presence.format_details();
                                                if (!state_str.empty()) presence.custom_state = state_str;
                                                if (!details_str.empty()) presence.custom_details = details_str;
                                                if (!splat2_presence.stage_image_uri.empty()) {
                                                    presence.custom_image_uri =
                                                        splat2_presence.stage_image_uri;
                                                }
                                            }
                                        }
                                    } catch (...) {
                                    }
                                    break;
                                case RpcGameService::None:
                                    break;
                            }
                        }

                        // Cache exactly what the one service probe contributed.
                        // Empty values are intentional: a failed/unsupported
                        // enrichment remains generic for the rest of this session.
                        cached_custom_details = presence.custom_details;
                        cached_custom_state = presence.custom_state;
                        cached_custom_image_uri = presence.custom_image_uri;
                    } else if (service != RpcGameService::None && enrichment_attempted) {
                        // Normal recurring polls are Coral-only. Reuse the
                        // original enrichment so Discord does not lose the useful
                        // data just because we stopped contacting the game API.
                        presence.custom_details = cached_custom_details;
                        presence.custom_state = cached_custom_state;
                        presence.custom_image_uri = cached_custom_image_uri;
                    }

                    // A sign-out clears Discord immediately. Do not allow a slow
                    // request from the old account to republish stale activity.
                    const auto latest = config_.snapshot();
                    if (!stopping_ && latest.discord_presence &&
                        latest.session_token == session_token &&
                        account_generation_.load() == generation) {
                        discord_.update(presence);
                    }
                } else {
                    reset_enrichment();
                    discord_.clear();
                }
            }
        } catch (const std::exception& error) {
            if (is_invalid_grant(error.what())) {
                invalidate_session(
                    "Nintendo Account session expired. Sign in again to continue.");
                continue;
            }
            if (!stopping_) {
                std::cerr << "Presence: " << error.what() << '\n';
            }
        }

        // Re-randomize every cycle so timer-driven Nintendo requests do not
        // land on an exact 60-second cadence. Explicit user-triggered refreshes
        // still bypass this wait and run immediately.
        const auto poll_interval = jittered_interval(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kPresencePollInterval));
        std::unique_lock wait_lock(presence_sleep_mutex_);
        presence_cv_.wait_for(wait_lock, poll_interval, [this] {
            if (stopping_) return true;
            if (presence_refresh_requested_.load(std::memory_order_acquire)) {
                return true;
            }
            const auto state = config_.snapshot();
            return !state.discord_presence || state.session_token.empty();
        });
    }
}

void App::auto_sync_loop() {
    while (!stopping_) {
        auto config = config_.snapshot();

        // v1.0.0 only ran the recurring timer while auto-sync was enabled and
        // an account was signed in. Wait indefinitely otherwise, then start a
        // fresh interval when the setting/account becomes active.
        if (!config.auto_sync || config.session_token.empty()) {
            std::unique_lock sleep_lock(sleep_mutex_);
            sleep_cv_.wait(sleep_lock, [this] {
                if (stopping_) return true;
                const auto state = config_.snapshot();
                return state.auto_sync && !state.session_token.empty();
            });
            continue;
        }

        const auto nominal_interval = std::chrono::minutes(
            std::max(1, config.sync_interval_minutes));
        const auto interval = jittered_interval(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                nominal_interval));
        std::unique_lock sleep_lock(sleep_mutex_);
        const auto wake_reason = sleep_cv_.wait_for(sleep_lock, interval);
        sleep_lock.unlock();
        if (stopping_) break;
        if (wake_reason != std::cv_status::timeout) continue;

        const auto latest = config_.snapshot();
        if (latest.auto_sync && !latest.session_token.empty()) {
            queue_sync(true);
        }
    }
}

void App::auth_callback_loop() {
    while (!stopping_) {
        if (const auto callback = take_nintendo_auth_callback()) {
            if (auth_pending_) complete_pending_login(*callback);
        }
        std::unique_lock sleep_lock(sleep_mutex_);
        sleep_cv_.wait_for(sleep_lock, kAuthCallbackPollInterval);
    }
}

void App::start_operational_workers() {
    bool expected = false;
    if (!operational_workers_started_.compare_exchange_strong(expected, true)) return;
    presence_thread_ = std::thread([this] { presence_loop(); });
    auto_sync_thread_ = std::thread([this] { auto_sync_loop(); });
}

void App::start_workers() {
    sync_thread_ = std::thread([this] { sync_loop(); });
    auth_callback_thread_ = std::thread([this] { auth_callback_loop(); });
    if (!config_.snapshot().session_token.empty()) start_operational_workers();
}

void App::request_stop() {
    if (stopping_.exchange(true)) return;
    auth_pending_ = false;
    unregister_nintendo_auth_protocol();
    clear_nintendo_auth_callback();
    wake_workers();
}

void App::request_exit() {
    bool expected = false;
    if (!explicit_exit_.compare_exchange_strong(expected, true)) return;

    // Start the bound before doing any cleanup: even local Discord IPC or OS
    // unregister calls can block unexpectedly. This guarantees the explicit
    // Exit command cannot leave a hidden process behind.
    std::thread([] {
        std::this_thread::sleep_for(kExitWatchdogDelay);
        std::_Exit(0);
    }).detach();

    request_stop();
    ui_.stop();
    discord_.clear();
}

void App::stop_workers() {
    request_stop();
    if (workers_joined_.exchange(true)) return;

    if (sync_thread_.joinable()) sync_thread_.join();
    if (auto_sync_thread_.joinable()) auto_sync_thread_.join();
    if (presence_thread_.joinable()) presence_thread_.join();
    if (auth_callback_thread_.joinable()) auth_callback_thread_.join();
    discord_.clear();
}

int App::run() {
    update_menu();
    PlatformCallbacks callbacks;

    callbacks.ready = [this] {
        const bool had_session = !config_.snapshot().session_token.empty();
        if (!had_session) sign_in_or_out();
        start_workers();
        const auto config = config_.snapshot();
        if (config.discord_presence && !config.session_token.empty()) {
            request_presence_refresh();
        }
        if (config.auto_sync && !config.session_token.empty()) {
            queue_sync(true);
        }
    };

    callbacks.sync_now = [this] {
        queue_sync(false);
        const auto config = config_.snapshot();
        if (config.discord_presence && !config.session_token.empty()) {
            request_presence_refresh();
        }
    };

    callbacks.toggle_auto = [this] {
        const auto config = config_.update([](AppConfig& value) {
            value.auto_sync = !value.auto_sync;
            value.auto_sync_setting_version = 1;
        });
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.auto_sync ? "Auto-sync enabled" : "Auto-sync disabled";
        }
        update_menu();
        sleep_cv_.notify_all();
        if (config.auto_sync && !config.session_token.empty()) {
            queue_sync(false);
        }
        if (config.notifications) {
            ui_.notify(
                "NSO Album Sync",
                config.auto_sync
                    ? "Auto-sync enabled (refreshes every hour)."
                    : "Auto-sync disabled.");
        }
    };

    callbacks.toggle_notifications = [this] {
        config_.update([](AppConfig& value) {
            value.notifications = !value.notifications;
        });
        update_menu();
    };

    callbacks.toggle_discord = [this] {
        const auto config = config_.update([](AppConfig& value) {
            value.discord_presence = !value.discord_presence;
            value.discord_presence_setting_version = 1;
        });
        if (config.discord_presence && !config.session_token.empty()) {
            request_presence_refresh();
        } else {
            presence_refresh_requested_.store(false, std::memory_order_release);
            discord_.clear();
            presence_cv_.notify_all();
        }
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.discord_presence
                ? "Discord presence enabled — visibility is controlled by Discord Activity Sharing"
                : "Discord presence disabled";
        }
        update_menu();
        if (config.discord_presence && config.notifications) {
            ui_.notify(
                "Discord Rich Presence",
                "Presence is enabled. In Discord, Activity Sharing must also "
                "be enabled for friends or server members to see it.");
        }
    };

    callbacks.select_folder = [this] {
        const auto current = config_.snapshot();
        const auto selected = ui_.choose_folder(current.destination_folder);
        if (!selected.empty()) {
            config_.update([&](AppConfig& value) {
                value.destination_folder = selected;
            });
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Album folder updated";
            }
            update_menu();
        }
    };

    callbacks.open_folder = [this] {
        open_path(config_.snapshot().destination_folder);
    };

    callbacks.toggle_start = [this] {
        set_start_on_boot(!start_on_boot_enabled());
        const bool enabled = start_on_boot_enabled();
        config_.update([enabled](AppConfig& value) {
            value.start_on_boot = enabled;
        });
        update_menu();
    };

    callbacks.proxy = [this] {
        const auto current = config_.snapshot();
        const auto proxy = ui_.prompt(
            "HTTP Proxy",
            "Enter a proxy URL.",
            current.proxy_url);
        const auto normalized = trim(proxy);
        config_.update([&](AppConfig& value) {
            value.proxy_url = normalized;
        });
        http_.set_proxy(normalized);
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = normalized.empty()
                ? "HTTP proxy disabled"
                : "HTTP proxy updated";
        }
        update_menu();
    };

    callbacks.sign_in_out = [this] { sign_in_or_out(); };
    callbacks.exit = [this] { request_exit(); };

    ui_.run(callbacks);
    stop_workers();
    return 0;
}

}  // namespace nso
