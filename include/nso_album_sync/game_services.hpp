#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nso {

// Use Nintendo's actual NookLink service ID when Album Sync has to acquire a
// GameWebServiceToken itself. This is the direct-client path proven by nxapi.
// nso-webapp's account broker may reuse/cache token material more broadly, but
// that broker behavior is not equivalent to blindly selecting an arbitrary
// locally cached token in the desktop client.
inline constexpr std::uint64_t kAnimalCrossingGameServiceId = 4953919198265344ULL;
inline constexpr std::uint64_t kAnimalCrossingNintendoGameServiceId = 4953919198265344ULL;
inline constexpr std::uint64_t kSmashBrosGameServiceId = 5598642853249024ULL;
inline constexpr std::uint64_t kSmashBrosGameServiceIdAlt = 5614999764533248ULL;
inline constexpr std::uint64_t kSplatoon2GameServiceId = 5741031244955648ULL;

struct AnimalCrossingPresence {
    std::string island_name;
    std::string resident_name;
    std::string native_fruit;
    std::string image_uri;
    bool active = false;

    std::string format_state() const {
        return island_name;
    }

    std::string format_details() const {
        if (resident_name.empty()) return island_name;
        if (island_name.empty()) return resident_name;
        return resident_name + " • " + island_name;
    }
};

struct SmashBrosPresence {
    // Smash World is session/cookie based. These fields are intentionally only
    // populated when a value is observed in a verified Nintendo response.
    std::string main_fighter;
    std::string smash_tag;
    std::int64_t gsp = 0;
    bool is_elite = false;
    std::string fighter_image_uri;
    bool active = false;

    std::string format_state() const {
        if (!main_fighter.empty()) return "Fighter: " + main_fighter;
        return {};
    }

    std::string format_details() const {
        return smash_tag.empty() ? std::string{} : "Smash Tag: " + smash_tag;
    }
};

struct Splatoon2Presence {
    std::string player_name;
    std::string weapon_name;
    std::string rank_name;
    std::int64_t player_level = 0;
    std::int64_t star_rank = 0;
    // The player's current weapon image is a supplementary small RPC image;
    // Coral's Splatoon 2 artwork remains the large image.
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const {
        std::string state;
        if (player_level > 0) {
            state = star_rank > 0
                ? "Level " + std::to_string(player_level) + " (Prestige " + std::to_string(star_rank) + ")"
                : "Level " + std::to_string(player_level);
        }
        if (!rank_name.empty()) {
            if (!state.empty()) state += " • ";
            state += rank_name;
        }
        return state;
    }

    std::string format_details() const {
        return player_name;
    }
};

class GameServicesClient {
public:
    explicit GameServicesClient(HttpClient& http);

    AnimalCrossingPresence fetch_animal_crossing_presence(const std::string& web_service_token);
    SmashBrosPresence fetch_smash_presence(const std::string& web_service_token);
    Splatoon2Presence fetch_splatoon2_presence(const std::string& web_service_token);

    void set_locale(const std::string& language, const std::string& country) {
        const auto next_language = language.empty() ? std::string("en-GB") : language;
        const auto next_country = country.empty() ? std::string("GB") : country;
        std::lock_guard lock(mutex_);
        if (language_ == next_language && country_ == next_country) return;
        language_ = next_language;
        country_ = next_country;
        sessions_.clear();
    }

    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string language_ = "en-GB";
    std::string country_ = "GB";

    struct ServiceSession {
        std::string source_token;
        std::string cookie;
        std::string user_id;
        std::string auth_token;
        bool user_auth_attempted = false;
        std::chrono::system_clock::time_point expires_at{};
    };

    std::unordered_map<std::string, ServiceSession> sessions_;
};

}  // namespace nso
