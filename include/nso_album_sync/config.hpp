#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kDiscordApplicationId = 1538902170433495172ULL;

struct AppConfig {
    std::string session_token;
    std::string user_nickname = "Nintendo Switch Player";
    std::string destination_folder;

    bool auto_sync = false;
    int auto_sync_setting_version = 1;
    bool notifications = false;
    bool discord_presence = false;
    int discord_presence_setting_version = 1;
    bool start_on_boot = false;

    int sync_interval_minutes = 60;
    std::string last_sync = "Never";

    std::string proxy_url;
    std::string nxapi_auth_client_id = "eJ8TDme0c-Z4czx5SvZabA";
    std::uint64_t discord_application_id = kDiscordApplicationId;
};

std::string default_album_folder();

class ConfigManager {
public:
    ConfigManager();

    // Thread-safe snapshot/update APIs. The application has UI, presence and
    // sync threads, so callers should not retain references to the live config.
    AppConfig snapshot() const;
    AppConfig update(const std::function<void(AppConfig&)>& updater);

    void load();
    void save();
    void clear_session();

    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path config_file_;
    AppConfig config_;
    mutable std::mutex mutex_;

    void save_locked();
};

}  // namespace nso
