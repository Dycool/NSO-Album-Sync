#include "nso_album_sync/app.hpp"

#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <thread>

namespace nso {
namespace {

constexpr auto kPresencePollInterval = std::chrono::seconds(60);
constexpr auto kAuthCallbackPollInterval = std::chrono::milliseconds(150);
constexpr auto kExitWatchdogDelay = std::chrono::seconds(5);

constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiDisclosure[] =
    "NSO Album Sync uses the third-party nxapi-znca-api service at "
    "fancy.org.uk for Nintendo Switch Online request attestation and "
    "request/response encryption.\n\n"
    "When Nintendo Switch Online features are used, your Nintendo Account "
    "id_token, Coral token, and the data sent to and received from the Coral "
    "API are sent to and processed by that third-party service. These tokens can contain "
    "Nintendo Account information and can authenticate Nintendo services while "
    "they remain valid.\n\n"
    "Service source, terms, and end-user information:\n"
    "https://github.com/samuelthomas2774/nxapi-znca-api\n\n"
    "Continue with Nintendo Account sign-in?";

constexpr char kManualLoginMessage[] =
    "Automatic return from the browser is unavailable on this system. "
    "Nintendo's sign-in page is open in your browser. Sign in, then "
    "right-click or copy the link behind “Select this person” and paste the "
    "complete redirect link below:";

constexpr char kPendingLoginMessage[] =
    "NSO Album Sync is waiting for Nintendo to return from your browser. "
    "Normally you only need to click “Select this person” in the browser.\n\n"
    "If the browser did not return to NSO Album Sync, paste the complete "
    "redirect link here as a manual fallback. Cancel to stop this sign-in.";

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

}  // namespace

App::App()
    : auth_(http_),
      nxapi_(
          http_,
          config_.snapshot().nxapi_auth_client_id,
          config_.directory() / "nxapi-cache.json"),
      coral_(http_, auth_, nxapi_, config_.directory()),
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
    nxapi_.clear_user_auth();
    auth_.clear_cached_tokens();
    config_.clear_session();
    config_.update([](AppConfig& config) {
        config.user_nickname = "Nintendo Switch Player";
        config.last_sync = "Never";
    });
    discord_.clear();
    {
        std::lock_guard state_lock(state_mutex_);
        last_sync_ = "Never";
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
            status_ = "Syncing Nintendo Switch album…";
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
                    std::to_string(result.new_downloads) +
                        (result.new_downloads == 1
                             ? " new capture downloaded."
                             : " new captures downloaded."));
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
        queue_sync(false);
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
        if (!ui_.confirm("Sign Out", "Disconnect the current Nintendo Account?")) {
            return;
        }
        account_generation_.fetch_add(1);
        coral_.clear_cached_session();
        nxapi_.clear_user_auth();
        auth_.clear_cached_tokens();
        config_.clear_session();
        config_.update([](AppConfig& config) {
            config.user_nickname = "Nintendo Switch Player";
            config.last_sync = "Never";
        });
        discord_.clear();
        {
            std::lock_guard state_lock(state_mutex_);
            last_sync_ = "Never";
            status_ = "Signed out";
        }
        update_menu();
        wake_workers();
        return;
    }

    if (auth_pending_) {
        const auto redirect = ui_.prompt(
            "Complete Nintendo Account Sign-In", kPendingLoginMessage);
        if (redirect.empty()) {
            auth_pending_ = false;
            unregister_nintendo_auth_protocol();
            clear_nintendo_auth_callback();
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Sign-in cancelled";
            }
            update_menu();
            return;
        }
        complete_pending_login(redirect);
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
    const bool automatic_callback = register_nintendo_auth_protocol();

    try {
        const auto authorize_url = auth_.authorize_url();
        auth_pending_ = true;
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = automatic_callback
                ? "Waiting for Nintendo Account sign-in in your browser…"
                : "Waiting for the Nintendo Account redirect link…";
        }
        update_menu();
        open_url(authorize_url);

        if (automatic_callback) return;

        const auto redirect = ui_.prompt(
            "Nintendo Account Sign-In", kManualLoginMessage);
        if (redirect.empty()) {
            auth_pending_ = false;
            unregister_nintendo_auth_protocol();
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Sign-in cancelled";
            }
            update_menu();
            return;
        }
        complete_pending_login(redirect);
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
    while (!stopping_) {
        auto config = config_.snapshot();
        if (!config.discord_presence || config.session_token.empty()) {
            discord_.clear();
            std::unique_lock wait_lock(presence_sleep_mutex_);
            presence_cv_.wait(wait_lock, [this] {
                if (stopping_) return true;
                const auto state = config_.snapshot();
                return state.discord_presence && !state.session_token.empty();
            });
            continue;
        }

        const auto generation = account_generation_.load();
        const auto session_token = config.session_token;
        try {
            const auto presence = coral_.self_presence(session_token);
            config = config_.snapshot();
            if (!stopping_ && config.discord_presence &&
                config.session_token == session_token &&
                account_generation_.load() == generation) {
                if (presence.is_playing()) discord_.update(presence);
                else discord_.clear();
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

        std::unique_lock wait_lock(presence_sleep_mutex_);
        presence_cv_.wait_for(wait_lock, kPresencePollInterval);
    }
}

void App::auto_sync_loop() {
    while (!stopping_) {
        const auto config = config_.snapshot();
        const auto interval = std::chrono::minutes(
            std::max(15, config.sync_interval_minutes));
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
        if (had_session && !config.session_token.empty() && config.auto_sync) {
            queue_sync(true);
        }
    };

    callbacks.sync_now = [this] { queue_sync(false); };

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
        if (config.notifications) {
            ui_.notify(
                "NSO Album Sync",
                config.auto_sync ? "Auto-sync enabled." : "Auto-sync disabled.");
        }
    };

    callbacks.toggle_notifications = [this] {
        const auto config = config_.update([](AppConfig& value) {
            value.notifications = !value.notifications;
        });
        update_menu();
        if (config.notifications) {
            ui_.notify(
                "NSO Album Sync",
                "Notifications are enabled and working.");
        }
    };

    callbacks.toggle_discord = [this] {
        const auto config = config_.update([](AppConfig& value) {
            value.discord_presence = !value.discord_presence;
            value.discord_presence_setting_version = 1;
        });
        if (!config.discord_presence) discord_.clear();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.discord_presence
                ? "Discord presence enabled — visibility is controlled by Discord Activity Sharing"
                : "Discord presence disabled";
        }
        update_menu();
        presence_cv_.notify_all();
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
            "HTTP proxy URL used for Nintendo, nxapi and media requests "
            "(leave blank to disable):",
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
