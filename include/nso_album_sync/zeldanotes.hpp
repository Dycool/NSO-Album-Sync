#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kZeldaNotesGameServiceId = 5935781783175168ULL;
inline constexpr std::uint64_t kZeldaNotesGameServiceIdAlt = 4974384874151936ULL;

enum class ZeldaGame {
    Unknown,
    BreathOfTheWild,
    TearsOfTheKingdom
};

enum class ZeldaLayer {
    Unknown,
    Sky,
    Surface,
    Depths
};

struct ZeldaPresence {
    ZeldaGame game = ZeldaGame::Unknown;
    ZeldaLayer layer = ZeldaLayer::Unknown;
    std::string region_name;
    std::string location_name;
    std::string activity_name;
    int shrines_completed = 0;
    int shrines_total = 152;
    int divine_beasts_completed = 0;
    int divine_beasts_total = 4;
    int lightroots_completed = 0;
    int lightroots_total = 120;
    int koroks_found = 0;
    int battery_cells = 0;
    int hearts = 0;
    double stamina_wheels = 0.0;
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const {
        if (!activity_name.empty()) {
            return activity_name;
        }

        if (game == ZeldaGame::BreathOfTheWild) {
            std::string state = "🌿 " + (region_name.empty() ? "Hyrule" : region_name);
            if (shrines_completed > 0) {
                state += " (" + std::to_string(shrines_completed) + "/" +
                         std::to_string(shrines_total) + " Shrines ⛩️)";
            }
            if (divine_beasts_completed > 0) {
                state += " [" + std::to_string(divine_beasts_completed) + "/" +
                         std::to_string(divine_beasts_total) + " Beasts 🐘]";
            }
            return state;
        }

        if (layer == ZeldaLayer::Sky) {
            std::string state = "☁️ The Sky Islands";
            if (!region_name.empty()) state += " (" + region_name + ")";
            return state;
        }
        if (layer == ZeldaLayer::Depths) {
            std::string state = "🌑 The Depths";
            if (lightroots_completed > 0) {
                state += " (" + std::to_string(lightroots_completed) + "/" +
                         std::to_string(lightroots_total) + " Lightroots 💡)";
            } else if (!region_name.empty()) {
                state += " (" + region_name + ")";
            }
            return state;
        }
        if (layer == ZeldaLayer::Surface || !region_name.empty()) {
            std::string state = "🌿 " + (region_name.empty() ? "Hyrule Surface" : region_name);
            if (shrines_completed > 0) {
                state += " (" + std::to_string(shrines_completed) + "/" +
                         std::to_string(shrines_total) + " Shrines ⛩️)";
            }
            return state;
        }
        if (shrines_completed > 0) {
            return "⛩️ " + std::to_string(shrines_completed) + "/" +
                   std::to_string(shrines_total) + " Shrines Completed";
        }
        return {};
    }

    std::string format_details() const {
        std::string details;
        if (!location_name.empty()) {
            details = "Exploring: " + location_name;
        }
        if (koroks_found > 0) {
            if (!details.empty()) details += " | ";
            details += "Koroks: " + std::to_string(koroks_found) + " 🍃";
        }
        if (game != ZeldaGame::BreathOfTheWild && battery_cells > 0) {
            if (!details.empty()) details += " | ";
            details += (battery_cells >= 8 ? "🔋 Energy: Max" : "🔋 Energy: " + std::to_string(battery_cells) + "/8");
        }
        if (hearts > 0) {
            if (!details.empty()) details += " | ";
            details += "❤️ x" + std::to_string(hearts);
        }
        if (game == ZeldaGame::BreathOfTheWild && stamina_wheels >= 1.0) {
            if (!details.empty()) details += " | ";
            details += "🟢 Stamina: " + std::to_string(static_cast<int>(stamina_wheels)) + " Wheels";
        }
        return details;
    }
};

class ZeldaNotesClient {
public:
    explicit ZeldaNotesClient(HttpClient& http);

    ZeldaPresence fetch_presence(const std::string& web_service_token);
    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string session_token_;
    std::string cached_web_service_token_;
    std::chrono::system_clock::time_point token_expiry_{};

    std::string ensure_token_locked(const std::string& web_service_token);
};

}  // namespace nso
