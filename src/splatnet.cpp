#include "nso_album_sync/splatnet.hpp"

#include <chrono>
#include <exception>
#include <vector>

namespace nso {
namespace {

constexpr char kBulletTokensUrl[] =
    "https://api.lp1.av5ja.srv.nintendo.net/api/bullet_tokens";
constexpr char kGraphQLUrl[] =
    "https://api.lp1.av5ja.srv.nintendo.net/api/graphql";
constexpr char kDefaultWebViewVersion[] = "6.0.0-f8f2b34a";
constexpr char kDefaultUserAgent[] =
    "Mozilla/5.0 (Linux; Android 12; Pixel 6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.4896.127 Mobile Safari/537.36";

constexpr auto kBulletTokenTtl = std::chrono::hours(2);

// Standard persisted query hashes for SplatNet 3 GraphQL queries
constexpr char kLatestBattleHistoriesHash[] =
    "80585ad4e4ecb674c3d8cd278adb1d21";
constexpr char kStageScheduleHash[] =
    "2b6940a02978cf47bc62e15e1233dbdf";

}  // namespace

SplatNetClient::SplatNetClient(HttpClient& http) : http_(http) {}

void SplatNetClient::clear_cache() {
    std::lock_guard lock(mutex_);
    bullet_token_.clear();
    cached_web_service_token_.clear();
    bullet_token_expiry_ = {};
}

std::string SplatNetClient::ensure_bullet_token_locked(
    const std::string& web_service_token) {
    const auto now = std::chrono::system_clock::now();
    if (!bullet_token_.empty() &&
        cached_web_service_token_ == web_service_token &&
        now < bullet_token_expiry_) {
        return bullet_token_;
    }

    bullet_token_.clear();
    cached_web_service_token_ = web_service_token;
    bullet_token_expiry_ = {};

    const std::vector<std::string> headers = {
        "X-Gamewebtoken: " + web_service_token,
        "Accept-Language: en-US",
        std::string("X-Web-View-Ver: ") + kDefaultWebViewVersion,
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    const auto response = http_.post(kBulletTokensUrl, "{}", headers, 15);
    if (response.status != 200 && response.status != 201) {
        return {};
    }

    try {
        const auto json = Json::parse(response.text());
        const auto token = json.string("bulletToken");
        if (!token.empty()) {
            bullet_token_ = token;
            bullet_token_expiry_ = now + kBulletTokenTtl;
            return bullet_token_;
        }
    } catch (...) {
    }

    return {};
}

SplatNetPresence SplatNetClient::fetch_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    std::string bullet_token;
    {
        std::lock_guard lock(mutex_);
        bullet_token = ensure_bullet_token_locked(web_service_token);
    }
    if (bullet_token.empty()) return {};

    const std::vector<std::string> headers = {
        "Authorization: Bearer " + bullet_token,
        "Accept-Language: en-US",
        std::string("X-Web-View-Ver: ") + kDefaultWebViewVersion,
        "Content-Type: application/json",
        std::string("User-Agent: ") + kDefaultUserAgent,
    };

    // 1. Try LatestBattleHistoriesQuery first for active match data
    try {
        const Json request_body(Json::object{
            {"variables", Json::object{}},
            {"extensions", Json::object{
                {"persistedQuery", Json::object{
                    {"version", 1.0},
                    {"sha256Hash", kLatestBattleHistoriesHash},
                }},
            }},
        });

        const auto response =
            http_.post(kGraphQLUrl, request_body.dump(), headers, 10);
        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            if (const auto* data = json.find("data"); data != nullptr) {
                if (const auto* history = data->find("latestBattleHistories");
                    history != nullptr) {
                    if (const auto* groups = history->find("historyGroups");
                        groups != nullptr) {
                        if (const auto* nodes = groups->find("nodes");
                            nodes != nullptr && nodes->is_array() &&
                            !nodes->as_array().empty()) {
                            const auto& group = nodes->as_array().front();
                            SplatNetPresence presence;
                            presence.active = true;

                            if (const auto* mode = group.find("bankaraMatchChallenge");
                                mode != nullptr && !mode->is_null()) {
                                presence.mode_name = "Anarchy Battle (Series)";
                            } else if (const auto* open = group.find("bankaraMatchOpen");
                                       open != nullptr && !open->is_null()) {
                                presence.mode_name = "Anarchy Battle (Open)";
                            } else if (const auto* xmatch = group.find("xMatch");
                                       xmatch != nullptr && !xmatch->is_null()) {
                                presence.mode_name = "X Battle";
                            } else if (const auto* regular = group.find("regularMatch");
                                       regular != nullptr && !regular->is_null()) {
                                presence.mode_name = "Regular Battle (Turf War)";
                            } else {
                                presence.mode_name = "Battle";
                            }

                            if (const auto* details = group.find("historyDetails");
                                details != nullptr) {
                                if (const auto* detail_nodes = details->find("nodes");
                                    detail_nodes != nullptr &&
                                    detail_nodes->is_array() &&
                                    !detail_nodes->as_array().empty()) {
                                    const auto& latest_match = detail_nodes->as_array().front();
                                    if (const auto* vs_rule = latest_match.find("vsRule");
                                        vs_rule != nullptr) {
                                        presence.rule_name = vs_rule->string("name");
                                    }
                                    if (const auto* vs_stage = latest_match.find("vsStage");
                                        vs_stage != nullptr) {
                                        presence.stage_name = vs_stage->string("name");
                                        if (const auto* img = vs_stage->find("image");
                                            img != nullptr) {
                                            presence.stage_image_uri = img->string("url");
                                        }
                                    }
                                }
                            }

                            if (!presence.mode_name.empty() || !presence.stage_name.empty()) {
                                return presence;
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    // 2. Fallback: Query current rotation schedule
    try {
        const Json schedule_request(Json::object{
            {"variables", Json::object{}},
            {"extensions", Json::object{
                {"persistedQuery", Json::object{
                    {"version", 1.0},
                    {"sha256Hash", kStageScheduleHash},
                }},
            }},
        });

        const auto response =
            http_.post(kGraphQLUrl, schedule_request.dump(), headers, 10);
        if (response.status == 200) {
            const auto json = Json::parse(response.text());
            if (const auto* data = json.find("data"); data != nullptr) {
                if (const auto* regular = data->find("regularSchedules");
                    regular != nullptr) {
                    if (const auto* nodes = regular->find("nodes");
                        nodes != nullptr && nodes->is_array() && !nodes->as_array().empty()) {
                        const auto& current_node = nodes->as_array().front();
                        if (const auto* match_setting = current_node.find("regularMatchSetting");
                            match_setting != nullptr) {
                            SplatNetPresence presence;
                            presence.active = true;
                            presence.mode_name = "Regular Battle";
                            if (const auto* rule = match_setting->find("vsRule"); rule != nullptr) {
                                presence.rule_name = rule->string("name");
                            }
                            if (const auto* stages = match_setting->find("vsStages");
                                stages != nullptr && stages->is_array() && !stages->as_array().empty()) {
                                const auto& stage = stages->as_array().front();
                                presence.stage_name = stage.string("name");
                                if (const auto* img = stage.find("image"); img != nullptr) {
                                    presence.stage_image_uri = img->string("url");
                                }
                            }
                            return presence;
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    return {};
}

}  // namespace nso
