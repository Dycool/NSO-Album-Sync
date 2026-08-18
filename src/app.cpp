#include "nso_album_sync/app.hpp"

#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

namespace nso {
namespace {

constexpr auto kPresencePollInterval = std::chrono::seconds(60);

constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiDisclosure[] =
    "NSO Album Sync uses the third-party nxapi-znca-api service at "
    "fancy.org.uk for Nintendo Switch Online request attestation and "
    "encryption/decryption.\n\n"
    "During sign-in, your Nintendo Account id_token is sent to that service. "
    "The token can contain Nintendo Account information and can be used to "
    "authenticate to Nintendo Switch Online services while it is valid.\n\n"
    "Service details and public API terms:\n"
    "https://github.com/samuelthomas2774/nxapi-znca-api\n\n"
    "Continue with Nintendo Account sign-in?";

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

}  // namespace

App::App()
    : auth_(http_),
      nxapi_(http_, config_.config().nxapi_auth_client_id),
      coral_(http_, auth_, nxapi_),
      sync_(config_, coral_, http_),
      discord_(config_.config().discord_application_id) {
    http_.set_proxy(config_.config().proxy_url);
    last_sync_ = config_.config().last_sync.empty()
        ? "Never"
        : config_.config().last_sync;
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

void App::sync_now(bool background) {
    if (!sync_mutex_.try_lock()) {
        if (!background) {
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Sync already running";
            }
            update_menu();
        }
        return;
    }

    std::unique_lock lock(sync_mutex_, std::adopt_lock);

    try {
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Syncing Nintendo Switch album…";
        }
        update_menu();

        const auto result = sync_.sync();
        const auto sync_time = current_time_text();
        {
            std::lock_guard state_lock(state_mutex_);
            last_sync_ = sync_time;
            status_ = "Ready";
        }
        config_.config().last_sync = sync_time;
        config_.save();

        if (config_.config().notifications) {
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
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = error.what();
        }

        if (config_.config().notifications) {
            ui_.notify("NSO Album Sync", error.what());
        }
    }

    update_menu();
}

void App::queue_sync(bool background) {
    if (stopping_) {
        return;
    }

    std::lock_guard lock(manual_workers_mutex_);
    manual_workers_.emplace_back([this, background] {
        sync_now(background);
    });
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

        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Signed out";
        }
        update_menu();
        wake_workers();
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

    try {
        open_url(auth_.authorize_url());

        const auto redirect = ui_.prompt(
            "Nintendo Account Sign-In",
            "Nintendo's sign-in page is open in your browser. Sign in, then "
            "right-click or copy the link behind “Select this person” and "
            "paste the complete redirect link below:");

        if (redirect.empty()) {
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Sign-in cancelled";
            }
            update_menu();
            return;
        }

        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Signing in to Nintendo Account…";
        }
        update_menu();

        const auto auth_result = auth_.complete_login(redirect);
        config.session_token = auth_result.session_token;
        config.user_nickname = auth_result.user_nickname;
        config_.save();

        {
            std::lock_guard state_lock(state_mutex_);
            status_ = "Connected as " + config.user_nickname;
        }
        update_menu();
        wake_workers();

        // v1 always performed a first sync after a successful sign-in, even
        // when recurring auto-sync was disabled.
        queue_sync(false);
    } catch (const std::exception& error) {
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
        sleep_cv_.wait_for(sleep_lock, kPresencePollInterval);
    }
}

void App::auto_sync_loop() {
    while (!stopping_) {
        const auto interval = std::chrono::minutes(
            std::max(1, config_.config().sync_interval_minutes));

        std::unique_lock sleep_lock(sleep_mutex_);
        const auto wake_reason = sleep_cv_.wait_for(sleep_lock, interval);
        sleep_lock.unlock();

        if (stopping_) {
            break;
        }

        // Setting changes wake the timer so the new interval takes effect, but
        // only an actual timer expiry performs a recurring sync. This avoids a
        // duplicate sync when sign-in wakes both the presence and timer workers.
        if (wake_reason != std::cv_status::timeout) {
            continue;
        }

        const auto& config = config_.config();
        if (config.auto_sync && !config.session_token.empty()) {
            sync_now(true);
        }
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

    {
        std::lock_guard lock(manual_workers_mutex_);
        for (auto& worker : manual_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        manual_workers_.clear();
    }

    discord_.clear();
}

int App::run() {
    update_menu();

    PlatformCallbacks callbacks;

    callbacks.ready = [this] {
        const bool had_session = !config_.config().session_token.empty();
        if (!had_session) {
            // Preserve v1 onboarding on the native UI thread. GTK and AppKit
            // modal controls must not be invoked from a background worker.
            sign_in_or_out();
        }

        start_workers();

        if (had_session && !config_.config().session_token.empty()) {
            // v1 performed one startup sync regardless of the recurring toggle.
            // A successful new sign-in already queued its own first sync.
            queue_sync(true);
        }
    };

    callbacks.sync_now = [this] {
        queue_sync(false);
    };

    callbacks.toggle_auto = [this] {
        auto& config = config_.config();
        config.auto_sync = !config.auto_sync;
        config_.save();

        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.auto_sync
                ? "Auto-sync enabled"
                : "Auto-sync disabled";
        }
        update_menu();
        wake_workers();

        if (config.notifications) {
            ui_.notify(
                "NSO Album Sync",
                config.auto_sync
                    ? "Auto-sync enabled."
                    : "Auto-sync disabled.");
        }
    };

    callbacks.toggle_notifications = [this] {
        auto& config = config_.config();
        config.notifications = !config.notifications;
        config_.save();
        update_menu();

        // This is both confirmation for the user and a real end-to-end test of
        // the native notification path. On macOS it also triggers the system
        // permission request at the moment the user opts in.
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
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config.discord_presence
                ? "Discord presence enabled — visibility is controlled by Discord Activity Sharing"
                : "Discord presence disabled";
        }
        update_menu();
        wake_workers();

        if (config.discord_presence && config.notifications) {
            ui_.notify(
                "Discord Rich Presence",
                "Presence is enabled. In Discord, Activity Sharing must also "
                "be enabled for friends or server members to see it.");
        }
    };

    callbacks.select_folder = [this] {
        const auto selected =
            ui_.choose_folder(config_.config().destination_folder);

        if (!selected.empty()) {
            config_.config().destination_folder = selected;
            config_.save();
            {
                std::lock_guard state_lock(state_mutex_);
                status_ = "Album folder updated";
            }
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
            "HTTP proxy URL used for Nintendo, nxapi and media requests "
            "(leave blank to disable):",
            config_.config().proxy_url);

        config_.config().proxy_url = trim(proxy);
        http_.set_proxy(config_.config().proxy_url);
        config_.save();
        {
            std::lock_guard state_lock(state_mutex_);
            status_ = config_.config().proxy_url.empty()
                ? "HTTP proxy disabled"
                : "HTTP proxy updated";
        }
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
