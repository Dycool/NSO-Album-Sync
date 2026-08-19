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
    std::string player_name;
    std::string player_id;
    std::string title;
    std::string weapon_name;
    std::string rank_name;
    std::int64_t player_level = 0;
    // This is the current weapon image returned by the authenticated player's
    // record. Discord renders it as the supplementary small image while Coral's
    // game artwork remains the large image.
    std::string stage_image_uri;
    bool active = false;

    std::string format_details() const {
        std::string details;
        if (!player_name.empty()) details = "Player: " + player_name;
        if (!title.empty()) {
            if (!details.empty()) details += " • ";
            details += title;
        }
        return details;
    }

    std::string format_state() const {
        std::string state;
        if (player_level > 0) state = "Level " + std::to_string(player_level);
        if (!rank_name.empty()) {
            if (!state.empty()) state += " • ";
            state += "Rank " + rank_name;
        }
        if (state.empty() && !title.empty()) state = title;
        return state;
    }
};

class SplatNetClient {
public:
    explicit SplatNetClient(HttpClient& http) : http_(http) {}

    SplatNetPresence fetch_presence(const std::string& web_service_token);

    void set_locale(const std::string& language, const std::string& country) {
        const auto next_language = language.empty() ? std::string("en-GB") : language;
        const auto next_country = country.empty() ? std::string("GB") : country;
        std::lock_guard lock(mutex_);
        if (account_language_ == next_language && account_country_ == next_country) return;
        account_language_ = next_language;
        account_country_ = next_country;
        source_web_token_.clear();
        bullet_token_.clear();
        language_ = next_language;
        bullet_expires_at_ = {};
    }

    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string source_web_token_;
    std::string bullet_token_;
    std::string account_language_ = "en-GB";
    std::string account_country_ = "GB";
    std::string language_ = "en-GB";
    std::chrono::system_clock::time_point bullet_expires_at_{};

    std::string ensure_bullet_token(const std::string& web_service_token);
};

}  // namespace nso
