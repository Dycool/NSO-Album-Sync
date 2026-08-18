#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kSplatoon3GameServiceId = 4834290508791808ULL;
inline constexpr std::uint64_t kSplatoon3GameServiceIdAlt = 4834290530795520ULL;

struct SplatNetPresence {
    std::string mode_name;
    std::string rule_name;
    std::string stage_name;
    std::string stage_image_uri;
    std::string weapon_name;
    bool active = false;

    std::string format_details() const {
        if (!rule_name.empty() && !stage_name.empty()) {
            return rule_name + " on " + stage_name;
        }
        if (!stage_name.empty()) return stage_name;
        if (!rule_name.empty()) return rule_name;
        return {};
    }

    std::string format_state() const {
        if (!mode_name.empty()) return mode_name;
        return {};
    }
};

class SplatNetClient {
public:
    explicit SplatNetClient(HttpClient& http);

    SplatNetPresence fetch_presence(const std::string& web_service_token);
    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string bullet_token_;
    std::string cached_web_service_token_;
    std::chrono::system_clock::time_point bullet_token_expiry_{};

    std::string ensure_bullet_token_locked(const std::string& web_service_token);
};

}  // namespace nso
