#pragma once

#include "nso_album_sync/config.hpp"
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/http.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace nso {

struct SyncResult {
    int total_found = 0;
    int new_downloads = 0;
};

class SyncEngine {
public:
    SyncEngine(ConfigManager& config, CoralClient& coral, HttpClient& http)
        : config_(config), coral_(coral), http_(http) {}

    SyncResult sync(const std::function<bool()>& cancelled = {});

private:
    ConfigManager& config_;
    CoralClient& coral_;
    HttpClient& http_;

    std::string resolve_game_folder(
        const std::filesystem::path& album_directory,
        const std::string& app_name) const;
};

}  // namespace nso
