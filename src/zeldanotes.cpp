#include "nso_album_sync/zeldanotes.hpp"

#include <chrono>
#include <exception>
#include <vector>

namespace nso {
namespace {

constexpr char kZeldaNotesBaseUrl[] =
    "https://api.lp1.87abc152.srv.nintendo.net";
constexpr char kDefaultWebViewVersion[] = "1.0.0";
constexpr char kDefaultUserAgent[] =
    "Mozilla/5.0 (Linux; Android 12; Pixel 6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.4896.127 Mobile Safari/537.36";

constexpr auto kTokenTtl = std::chrono::hours(2);

}  // namespace

ZeldaNotesClient::ZeldaNotesClient(HttpClient& http) : http_(http) {}

void ZeldaNotesClient::clear_cache() {
    std::lock_guard lock(mutex_);
    session_token_.clear();
    cached_web_service_token_.clear();
    token_expiry_ = {};
}

std::string ZeldaNotesClient::ensure_token_locked(
    const std::string& web_service_token) {
    const auto now = std::chrono::system_clock::now();
    if (!session_token_.empty() &&
        cached_web_service_token_ == web_service_token &&
        now < token_expiry_) {
        return session_token_;
    }

    session_token_ = web_service_token;
    cached_web_service_token_ = web_service_token;
    token_expiry_ = now + kTokenTtl;
    return session_token_;
}

ZeldaPresence ZeldaNotesClient::fetch_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        token = ensure_token_locked(web_service_token);
    }
    if (token.empty()) return {};

    const std::vector<std::string> headers = {
        "X-Gamewebtoken: " + token,
        "Authorization: Bearer " + token,
        "Accept-Language: en-US",
        std::string("X-Web-View-Ver: ") + kDefaultWebViewVersion,
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    // Query Zelda Notes play data endpoint
    try {
        const auto response = http_.get(
            std::string(kZeldaNotesBaseUrl) + "/api/v1/play_data",
            headers,
            10);

        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            ZeldaPresence presence;
            presence.active = true;

            const auto game_title = json.string("gameTitle");
            if (game_title.find("Breath of the Wild") != std::string::npos ||
                game_title.find("ブレス オブ ザ ワイルド") != std::string::npos) {
                presence.game = ZeldaGame::BreathOfTheWild;
                presence.shrines_total = 120;
            } else if (game_title.find("Tears of the Kingdom") != std::string::npos ||
                       game_title.find("ティアーズ オブ ザ キングダム") != std::string::npos) {
                presence.game = ZeldaGame::TearsOfTheKingdom;
                presence.shrines_total = 152;
            }

            if (const auto* play_data = json.find("playData"); play_data != nullptr) {
                presence.shrines_completed = static_cast<int>(
                    play_data->integer("completedShrines", 0));
                if (const auto total = play_data->integer("totalShrines", 0); total > 0) {
                    presence.shrines_total = static_cast<int>(total);
                }
                presence.divine_beasts_completed = static_cast<int>(
                    play_data->integer("completedDivineBeasts", 0));
                presence.stamina_wheels = play_data->number("staminaWheels", 0.0);
                presence.lightroots_completed = static_cast<int>(
                    play_data->integer("activatedLightroots", 0));
                if (const auto roots = play_data->integer("totalLightroots", 0); roots > 0) {
                    presence.lightroots_total = static_cast<int>(roots);
                }
                presence.koroks_found = static_cast<int>(
                    play_data->integer("collectedKoroks", 0));
                presence.battery_cells = static_cast<int>(
                    play_data->integer("batteryEnergyCells", 0));
                presence.hearts = static_cast<int>(
                    play_data->integer("hearts", 0));
            }

            if (const auto* nav = json.find("navigation"); nav != nullptr) {
                const auto layer_str = nav->string("currentLayer");
                if (layer_str == "SKY") {
                    presence.layer = ZeldaLayer::Sky;
                } else if (layer_str == "DEPTHS") {
                    presence.layer = ZeldaLayer::Depths;
                } else if (layer_str == "SURFACE") {
                    presence.layer = ZeldaLayer::Surface;
                }

                presence.region_name = nav->string("currentRegion");
                presence.location_name = nav->string("currentLocation");
                presence.stage_image_uri = nav->string("imageUri");
            }

            if (const auto* event = json.find("activeEvent"); event != nullptr) {
                presence.activity_name = event->string("title");
            } else if (const auto* action = json.find("currentAction"); action != nullptr) {
                presence.activity_name = action->string("name");
            }

            return presence;
        }
    } catch (...) {
    }

    return {};
}

}  // namespace nso
