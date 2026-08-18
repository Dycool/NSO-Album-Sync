#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nso {

inline constexpr std::uint64_t kAnimalCrossingGameServiceId = 4953919198265344ULL;
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
        if (!island_name.empty()) {
            std::string state = "🏝️ Island: " + island_name;
            if (!native_fruit.empty()) state += " (Native: " + native_fruit + ")";
            return state;
        }
        return {};
    }

    std::string format_details() const {
        if (!resident_name.empty()) {
            return "Resident: " + resident_name;
        }
        return {};
    }
};

struct SmashBrosPresence {
    std::string main_fighter;
    std::string smash_tag;
    std::int64_t gsp = 0;
    bool is_elite = false;
    std::string fighter_image_uri;
    bool active = false;

    std::string format_state() const {
        if (!main_fighter.empty()) {
            std::string state = "⚔️ Main: " + main_fighter;
            if (gsp > 0) {
                const auto gsp_millions = static_cast<double>(gsp) / 1'000'000.0;
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), " (GSP: %.1fM", gsp_millions);
                state += buffer;
                if (is_elite) state += " ⭐ Elite";
                state += ")";
            }
            return state;
        }
        return {};
    }

    std::string format_details() const {
        std::string details;
        if (!smash_tag.empty()) {
            details = "Smash Tag: " + smash_tag;
        }
        if (!details.empty()) details += " | ";
        details += "Online Battles";
        return details;
    }
};

struct Splatoon2Presence {
    std::string mode_name;
    std::string rule_name;
    std::string stage_name;
    std::string weapon_name;
    std::string rank_name;
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const {
        std::string state;
        if (!mode_name.empty()) state = mode_name;
        if (!rule_name.empty()) {
            state = state.empty() ? rule_name : state + " (" + rule_name + ")";
        }
        if (!rank_name.empty()) {
            state = state.empty() ? "Rank " + rank_name : state + " • Rank " + rank_name;
        }
        return state;
    }

    std::string format_details() const {
        std::string details;
        if (!stage_name.empty()) {
            details = stage_name + " (4 vs 4)";
        }
        if (!weapon_name.empty()) {
            if (!details.empty()) details += " | ";
            details += weapon_name;
        }
        return details;
    }
};

class GameServicesClient {
public:
    explicit GameServicesClient(HttpClient& http);

    AnimalCrossingPresence fetch_animal_crossing_presence(const std::string& web_service_token);
    SmashBrosPresence fetch_smash_presence(const std::string& web_service_token);
    Splatoon2Presence fetch_splatoon2_presence(const std::string& web_service_token);

    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;

    struct TokenCache {
        std::string token;
        std::chrono::system_clock::time_point expires_at{};
    };

    std::unordered_map<std::string, TokenCache> token_cache_;

    std::string get_cached_token_locked(
        const std::string& service_key,
        const std::string& web_service_token);
};

}  // namespace nso
