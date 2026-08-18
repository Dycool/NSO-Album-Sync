#pragma once

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/discord.hpp"
#include "nso_album_sync/http.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"
#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/sync.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nso {

class App {
public:
    App();
    ~App();

    int run();

private:
    ConfigManager config_;
    HttpClient http_;
    NintendoAuthManager auth_;
    NxapiClient nxapi_;
    CoralClient coral_;
    SyncEngine sync_;
    DiscordPresence discord_;
    PlatformUi ui_;

    std::atomic<bool> stopping_{false};
    std::thread auto_sync_thread_;
    std::thread presence_thread_;

    // Manual/startup syncs run off the UI thread but are retained and joined at
    // shutdown so the App object can never be destroyed underneath a detached
    // worker.
    std::mutex manual_workers_mutex_;
    std::vector<std::thread> manual_workers_;

    // Album sync may be requested by both the timer and the tray menu.
    std::mutex sync_mutex_;

    // Both workers share a condition variable so settings changes and shutdown
    // can wake them immediately instead of waiting for the next timer tick.
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;

    std::mutex state_mutex_;
    std::string last_sync_ = "Never";
    std::string status_ = "Ready";

    void update_menu();
    void sync_now(bool background);
    void queue_sync(bool background);
    void sign_in_or_out();

    void start_workers();
    void stop_workers();
    void presence_loop();
    void auto_sync_loop();
    void wake_workers();
};

}  // namespace nso
