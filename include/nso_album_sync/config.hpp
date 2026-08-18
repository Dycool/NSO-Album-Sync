#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace nso {

struct AppConfig {
    std::string session_token;
    std::string user_nickname = "Nintendo Switch Player";
    std::string destination_folder;

    bool auto_sync = true;
    bool notifications = false;
    bool discord_presence = true;
    bool start_on_boot = false;

    int sync_interval_minutes = 60;
    std::string last_sync = "Never";

    std::string proxy_url;
    std::string nxapi_auth_client_id = "eJ8TDme0c-Z4czx5SvZabA";
    std::uint64_t discord_application_id = 637692124539650048ULL;
};

class ConfigManager {
public:
    ConfigManager();

    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }

    void load();
    void save();
    void clear_session();

    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path config_file_;
    AppConfig config_;
    std::mutex mutex_;
};

}  // namespace nso
