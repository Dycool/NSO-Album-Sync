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
    std::atomic<bool> auth_pending_{false};
    std::atomic<bool> operational_workers_started_{false};
    std::thread auto_sync_thread_;
    std::thread presence_thread_;
    std::thread auth_callback_thread_;

    std::mutex manual_workers_mutex_;
    std::vector<std::thread> manual_workers_;
    std::mutex sync_mutex_;
    std::mutex auth_flow_mutex_;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;

    std::mutex state_mutex_;
    std::string last_sync_ = "Never";
    std::string status_ = "Ready";

    void update_menu();
    void sync_now(bool background);
    void queue_sync(bool background);
    void sign_in_or_out();
    void complete_pending_login(const std::string& redirect_url_or_code);

    void start_workers();
    void start_operational_workers();
    void stop_workers();
    void presence_loop();
    void auto_sync_loop();
    void auth_callback_loop();
    void wake_workers();
};

}  // namespace nso
