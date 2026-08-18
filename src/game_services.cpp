#include "nso_album_sync/game_services.hpp"

#include <chrono>
#include <exception>
#include <vector>

namespace nso {
namespace {

constexpr char kNookLinkBaseUrl[] =
    "https://web.sd.lp1.acbaa.srv.nintendo.net";
constexpr char kSmashWorldBaseUrl[] =
    "https://app.smashbros.nintendo.net";
constexpr char kSplatNet2BaseUrl[] =
    "https://app.splatoon2.nintendo.net";

constexpr char kDefaultUserAgent[] =
    "Mozilla/5.0 (Linux; Android 12; Pixel 6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.4896.127 Mobile Safari/537.36";

constexpr auto kTokenTtl = std::chrono::hours(2);

std::string decode_fruit_emoji(int fruit_type) {
    switch (fruit_type) {
        case 0: return "🍎";
        case 1: return "🍒";
        case 2: return "🍊";
        case 3: return "🍑";
        case 4: return "🍐";
        default: return {};
    }
}

}  // namespace

GameServicesClient::GameServicesClient(HttpClient& http) : http_(http) {}

void GameServicesClient::clear_cache() {
    std::lock_guard lock(mutex_);
    token_cache_.clear();
}

std::string GameServicesClient::get_cached_token_locked(
    const std::string& service_key,
    const std::string& web_service_token) {
    const auto now = std::chrono::system_clock::now();
    const auto it = token_cache_.find(service_key);
    if (it != token_cache_.end() &&
        it->second.token == web_service_token &&
        now < it->second.expires_at) {
        return it->second.token;
    }

    token_cache_[service_key] = {web_service_token, now + kTokenTtl};
    return web_service_token;
}

AnimalCrossingPresence GameServicesClient::fetch_animal_crossing_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        token = get_cached_token_locked("acnh", web_service_token);
    }

    const std::vector<std::string> headers = {
        "Authorization: Bearer " + token,
        "Accept-Language: en-US",
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    try {
        const auto response = http_.get(
            std::string(kNookLinkBaseUrl) + "/api/v1/users",
            headers,
            10);

        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            if (const auto* users = json.find("users");
                users != nullptr && users->is_array() && !users->as_array().empty()) {
                const auto& user = users->as_array().front();
                AnimalCrossingPresence presence;
                presence.active = true;
                presence.resident_name = user.string("name");

                if (const auto* land = user.find("land"); land != nullptr) {
                    presence.island_name = land->string("name");
                    if (const auto* fruit = land->find("fruit"); fruit != nullptr) {
                        const auto fruit_id = static_cast<int>(fruit->integer("type", -1));
                        presence.native_fruit = decode_fruit_emoji(fruit_id);
                    }
                }

                if (const auto* photo = user.find("image"); photo != nullptr) {
                    presence.image_uri = photo->string("url");
                }

                return presence;
            }
        }
    } catch (...) {
    }

    return {};
}

SmashBrosPresence GameServicesClient::fetch_smash_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        token = get_cached_token_locked("smash", web_service_token);
    }

    const std::vector<std::string> headers = {
        "Authorization: Bearer " + token,
        "Accept-Language: en-US",
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    try {
        const auto response = http_.get(
            std::string(kSmashWorldBaseUrl) + "/api/v1/users/me",
            headers,
            10);

        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            SmashBrosPresence presence;
            presence.active = true;
            presence.smash_tag = json.string("screenName");

            if (const auto* fighter = json.find("fighter"); fighter != nullptr) {
                presence.main_fighter = fighter->string("name");
                presence.fighter_image_uri = fighter->string("imageUri");
            }

            if (const auto* rating = json.find("rating"); rating != nullptr) {
                presence.gsp = rating->integer("gsp", 0);
                presence.is_elite = rating->boolean("isElite", false);
            }

            if (!presence.main_fighter.empty() || !presence.smash_tag.empty()) {
                return presence;
            }
        }
    } catch (...) {
    }

    return {};
}

Splatoon2Presence GameServicesClient::fetch_splatoon2_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        token = get_cached_token_locked("splat2", web_service_token);
    }

    const std::vector<std::string> headers = {
        "X-Gamewebtoken: " + token,
        "Accept-Language: en-US",
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    try {
        const auto response = http_.get(
            std::string(kSplatNet2BaseUrl) + "/api/results",
            headers,
            10);

        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            if (const auto* results = json.find("results");
                results != nullptr && results->is_array() && !results->as_array().empty()) {
                const auto& latest = results->as_array().front();
                Splatoon2Presence presence;
                presence.active = true;

                if (const auto* game_mode = latest.find("game_mode"); game_mode != nullptr) {
                    presence.mode_name = game_mode->string("name");
                }

                if (const auto* rule = latest.find("rule"); rule != nullptr) {
                    presence.rule_name = rule->string("name");
                }

                if (const auto* stage = latest.find("stage"); stage != nullptr) {
                    presence.stage_name = stage->string("name");
                    presence.stage_image_uri = stage->string("image");
                }

                if (const auto* weapon = latest.find("player_result"); weapon != nullptr) {
                    if (const auto* wp = weapon->find("player"); wp != nullptr) {
                        if (const auto* wp_info = wp->find("weapon"); wp_info != nullptr) {
                            presence.weapon_name = wp_info->string("name");
                        }
                    }
                }

                if (const auto* udemae = latest.find("udemae"); udemae != nullptr) {
                    presence.rank_name = udemae->string("name");
                }

                return presence;
            }
        }
    } catch (...) {
    }

    return {};
}

}  // namespace nso
