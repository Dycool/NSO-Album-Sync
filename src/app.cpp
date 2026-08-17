#include "nso_album_sync/app.hpp"

#include "nso_album_sync/util.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

namespace nso {
namespace {

constexpr auto kPresencePollInterval = std::chrono::seconds(60);
constexpr auto kAutoSyncInterval = std::chrono::hours(1);

std::string current_time_text() {
    const auto now = std::time(nullptr);
    std::tm local_time{};

#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local_time);
    return buffer;
}

}  // namespace

App::App()
    : auth_(http_),
      nxapi_(http_, config_.config().nxapi_auth_client_id),
      coral_(http_, auth_, nxapi_),
      sync_(config_, coral_, http_),
      discord_(config_.config().discord_application_id) {
    http_.set_proxy(config_.config().proxy_url);
}

App::~App() {
    stop_workers();
}

void App::wake_workers() {
    sleep_cv_.notify_all();
}

void App::update_menu() {
    const auto& config = config_.config();

    MenuState menu;
    menu.nickname = config.user_nickname;
    menu.last_sync = last_sync_;
    menu.status = status_;
    menu.auto_sync = config.auto_sync;
    menu.notifications = config.notifications;
    menu.discord = config.discord_presence;
    menu.start_on_boot = start_on_boot_enabled();
    menu.signed_in = !config.session_token.empty();

    ui_.update(menu);
}

void App::sync_now() {
    if (!sync_mutex_.try_lock()) {
        status_ = "Sync already running";
        update_menu();
        return;
    }

    std::unique_lock lock(sync_mutex_, std::adopt_lock);

    try {
        status_ = "Syncing…";
        update_menu();

        const auto result = sync_.sync();
        last_sync_ = current_time_text();
        status_ = "Ready";

        if (config_.config().notifications && result.new_downloads > 0) {
            ui_.notify(
                "NSO Album Sync",
                std::to_string(result.new_downloads) +
                    " new capture(s) downloaded.");
        }
    } catch (const std::exception& error) {
        status_ = error.what();

        if (config_.config().notifications) {
            ui_.notify("NSO Album Sync", status_);
        }
    }

    update_menu();
}

void App::sign_in_or_out() {
    auto& config = config_.config();

    if (!config.session_token.empty()) {
        const bool confirmed = ui_.confirm(
            "Sign Out",
            "Disconnect the current Nintendo Account?");

        if (!confirmed) {
            return;
        }

        config_.clear_session();
        config.user_nickname = "Nintendo Switch Player";
        discord_.clear();
        config_.save();

        status_ = "Signed out";
        update_menu();
        return;
    }

    try {
        open_url(auth_.authorize_url());

        const auto redirect = ui_.prompt(
            "Nintendo Sign-In",
            "Sign in in your browser, copy the link behind “Select this person”, "
            "then paste it here:");

        if (redirect.empty()) {
            return;
        }

        status_ = "Signing in…";
        update_menu();

        const auto auth_result = auth_.complete_login(redirect);
        config.session_token = auth_result.session_token;
        config.user_nickname = auth_result.user_nickname;
        config_.save();

        status_ = "Connected";
        update_menu();
        wake_workers();
    } catch (const std::exception& error) {
        status_ = error.what();
        update_menu();
        ui_.notify("Nintendo Sign-In", status_);
    }
}

void App::presence_loop() {
    while (!stopping_) {
        const auto& config = config_.config();

        if (config.discord_presence && !config.session_token.empty()) {
            try {
                const auto presence = coral_.self_presence(config.session_token);

                if (presence.is_playing()) {
                    discord_.update(presence);
                } else {
                    discord_.clear();
                }
            } catch (const std::exception& error) {
                // Presence is optional. A failure here must never break album sync.
                std::cerr << "Presence: " << error.what() << '\n';
            }
        } else {
            discord_.clear();
        }

        std::unique_lock sleep_lock(sleep_mutex_);
        sleep_cv_.wait_for(
            sleep_lock,
            kPresencePollInterval,
            [this] { return stopping_.load(); });
    }
}

void App::auto_sync_loop() {
    while (!stopping_) {
        const auto& config = config_.config();

        if (config.auto_sync && !config.session_token.empty()) {
            sync_now();
        }

        std::unique_lock sleep_lock(sleep_mutex_);
        sleep_cv_.wait_for(
            sleep_lock,
            kAutoSyncInterval,
            [this] { return stopping_.load(); });
    }
}

void App::start_workers() {
    presence_thread_ = std::thread([this] { presence_loop(); });
    auto_sync_thread_ = std::thread([this] { auto_sync_loop(); });
}

void App::stop_workers() {
    if (stopping_.exchange(true)) {
        return;
    }

    wake_workers();

    if (auto_sync_thread_.joinable()) {
        auto_sync_thread_.join();
    }

    if (presence_thread_.joinable()) {
        presence_thread_.join();
    }

    discord_.clear();
}

int App::run() {
    update_menu();

    PlatformCallbacks callbacks;

    callbacks.ready = [this] { start_workers(); };

    callbacks.sync_now = [this] {
        // Keep the platform UI responsive while the network sync runs.
        std::thread([this] { sync_now(); }).detach();
    };

    callbacks.toggle_auto = [this] {
        auto& config = config_.config();
        config.auto_sync = !config.auto_sync;
        config_.save();
        update_menu();
        wake_workers();
    };

    callbacks.toggle_notifications = [this] {
        auto& config = config_.config();
        config.notifications = !config.notifications;
        config_.save();
        update_menu();

        // This is both confirmation for the user and a real end-to-end test of
        // the native notification path.  On macOS it also triggers the system
        // permission request at the moment the user opts in, rather than much
        // later when a sync finishes in the background.
        if (config.notifications) {
            ui_.notify(
                "NSO Album Sync",
                "Notifications are enabled and working.");
        }
    };

    callbacks.toggle_discord = [this] {
        auto& config = config_.config();
        config.discord_presence = !config.discord_presence;

        if (!config.discord_presence) {
            discord_.clear();
        }

        config_.save();
        update_menu();
        wake_workers();
    };

    callbacks.select_folder = [this] {
        const auto selected =
            ui_.choose_folder(config_.config().destination_folder);

        if (!selected.empty()) {
            config_.config().destination_folder = selected;
            config_.save();
            update_menu();
        }
    };

    callbacks.open_folder = [this] {
        open_path(config_.config().destination_folder);
    };

    callbacks.toggle_start = [this] {
        set_start_on_boot(!start_on_boot_enabled());
        config_.config().start_on_boot = start_on_boot_enabled();
        config_.save();
        update_menu();
    };

    callbacks.proxy = [this] {
        const auto proxy = ui_.prompt(
            "HTTP Proxy",
            "HTTP proxy URL (leave blank to disable):",
            config_.config().proxy_url);

        config_.config().proxy_url = trim(proxy);
        http_.set_proxy(config_.config().proxy_url);
        config_.save();
        update_menu();
    };

    callbacks.sign_in_out = [this] { sign_in_or_out(); };
    callbacks.exit = [this] {
        stop_workers();
        ui_.stop();
    };

    ui_.run(callbacks);
    stop_workers();
    return 0;
}

}  // namespace nso
