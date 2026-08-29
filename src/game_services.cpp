#include "nso_album_sync/game_services.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr char kNookLinkBaseUrl[] = "https://web.sd.lp1.acbaa.srv.nintendo.net";
constexpr char kSplatNet2BaseUrl[] = "https://app.splatoon2.nintendo.net";
constexpr char kWebServiceUserAgent[] =
    "Mozilla/5.0 (Linux; Android 8.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.125 Mobile Safari/537.36";
constexpr char kNxapiWebServiceUserAgent[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 15_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.3 Mobile/15E148 Safari/604.1";
constexpr char kBlancoVersion[] = "2.1.1";
constexpr auto kSessionTtl = std::chrono::minutes(90);

void log_nooklink_failure(const std::string& stage) {
    // Deliberately stage/status only. Never log GameWebServiceToken, _gtoken,
    // per-user auth tokens, Nintendo user IDs, cookies or profile payloads.
    const auto line = std::string("[NookLink RPC] ") + stage;
    std::cerr << line << '\n';
    try {
        static std::mutex log_mutex;
        std::lock_guard lock(log_mutex);
        const auto path = std::filesystem::temp_directory_path() /
            "nso-album-sync-rpc.log";
        std::ofstream output(path, std::ios::app);
        if (output) output << line << '\n';
    } catch (...) {
    }
}

std::string launch_url(
    const char* base,
    const std::string& language,
    const std::string& country) {
    return std::string(base) + "/?lang=" + language +
        "&na_country=" + country + "&na_lang=" + language;
}

std::string header_value(const HttpResponse& response, const std::string& key) {
    const auto it = response.headers.find(key);
    return it == response.headers.end() ? std::string{} : it->second;
}

std::vector<std::string> set_cookie_lines(const HttpResponse& response) {
    const auto cookies = header_value(response, "set-cookie");
    if (cookies.empty()) return {};
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= cookies.size()) {
        const auto end = cookies.find('\n', start);
        auto line = cookies.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string cookie_value(const HttpResponse& response, const std::string& name) {
    const auto needle = name + "=";
    for (const auto& line : set_cookie_lines(response)) {
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        if (line.compare(start, needle.size(), needle) != 0) continue;
        const auto value_start = start + needle.size();
        auto end = line.find(';', value_start);
        if (end == std::string::npos) end = line.size();
        return line.substr(value_start, end - value_start);
    }
    return {};
}

std::string trim_cookie_piece(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string merge_cookie_header(
    const std::string& existing,
    const HttpResponse& response) {
    std::map<std::string, std::string> cookies;

    const auto add_cookie_pair = [&cookies](std::string pair) {
        pair = trim_cookie_piece(std::move(pair));
        if (pair.empty()) return;
        const auto eq = pair.find('=');
        if (eq == std::string::npos || eq == 0) return;
        auto name = trim_cookie_piece(pair.substr(0, eq));
        auto value = trim_cookie_piece(pair.substr(eq + 1));
        if (!name.empty()) cookies[std::move(name)] = std::move(value);
    };

    std::size_t start = 0;
    while (start < existing.size()) {
        const auto end = existing.find(';', start);
        add_cookie_pair(existing.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }

    for (const auto& line : set_cookie_lines(response)) {
        const auto end = line.find(';');
        add_cookie_pair(line.substr(0, end));
    }

    std::string result;
    for (const auto& [name, value] : cookies) {
        if (!result.empty()) result += "; ";
        result += name + "=" + value;
    }
    return result;
}

std::vector<std::string> nooklink_api_headers(
    const std::string& cookie_header,
    const std::string& language,
    const std::string& country) {
    // Match the currently working nso-worker-backend request shape for NookLink
    // API calls. In particular, preserve the WebService browser fingerprint and
    // platform/origin context on POST /auth_token rather than using the older
    // minimal direct-client header set.
    return {
        std::string("User-Agent: ") + kNxapiWebServiceUserAgent,
        "Cookie: " + cookie_header,
        "Upgrade-Insecure-Requests: 1",
        "dnt: 1",
        "Accept: application/json, text/plain, */*",
        "Accept-Language: " + (language.empty() ? std::string("en-GB") : language),
        std::string("Origin: ") + kNookLinkBaseUrl,
        std::string("Referer: ") + kNookLinkBaseUrl + "/",
        "Content-Type: application/json",
        "X-Blanco-Version: " + std::string(kBlancoVersion),
        "x-appplatform: android",
        "x-appcolorscheme: DARK",
        "X-NACountry: " + (country.empty() ? std::string("GB") : country),
        "X-Requested-With: com.nintendo.znca",
    };
}

std::string html_attribute(const std::string& body, const std::string& name) {
    for (const char quote : {'\"', '\''}) {
        const std::string needle = name + "=" + quote;
        const auto begin = body.find(needle);
        if (begin == std::string::npos) continue;
        const auto value_begin = begin + needle.size();
        const auto end = body.find(quote, value_begin);
        if (end != std::string::npos) return body.substr(value_begin, end - value_begin);
    }
    return {};
}

std::vector<std::string> bootstrap_headers(
    const std::string& web_service_token,
    const std::string& language,
    const char* user_agent = kWebServiceUserAgent,
    bool use_account_accept_language = false) {
    const auto accept_language = use_account_accept_language
        ? language
        : std::string("en-GB,en-US;q=0.8");
    return {
        "Upgrade-Insecure-Requests: 1",
        std::string("User-Agent: ") + user_agent,
        "x-appplatform: android",
        "x-appcolorscheme: DARK",
        "x-gamewebtoken: " + web_service_token,
        "dnt: 1",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: " + accept_language,
        "X-Requested-With: com.nintendo.znca",
    };
}

std::string rank_value(const Json& player, const char* key, const char* short_name) {
    const auto* rank = player.find(key);
    if (!rank || !rank->is_object()) return {};
    if (rank->boolean("is_x", false)) return std::string(short_name) + " X";
    auto name = rank->string("name");
    if (name.empty()) return {};
    if (name == "S+") {
        // SplatNet's `number` is an internal rank code (X can report 128), not
        // the visible S+ suffix. Only append the dedicated S+ value when the
        // response actually supplies it; otherwise "S+" is the truthful value.
        const auto* s_plus_number = rank->find("s_plus_number");
        if (s_plus_number && s_plus_number->is_number()) {
            const auto number = s_plus_number->as_i64();
            if (number >= 0) name += std::to_string(number);
        }
    }
    return std::string(short_name) + " " + name;
}

}  // namespace

GameServicesClient::GameServicesClient(HttpClient& http) : http_(http) {}

void GameServicesClient::clear_cache() {
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

AnimalCrossingPresence GameServicesClient::fetch_animal_crossing_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) {
        log_nooklink_failure("no GameWebServiceToken");
        return {};
    }

    ServiceSession session;
    std::string language;
    std::string country;
    {
        std::lock_guard lock(mutex_);
        language = language_;
        country = country_;
        const auto it = sessions_.find("nooklink");
        if (it != sessions_.end() && it->second.source_token == web_service_token &&
            std::chrono::system_clock::now() < it->second.expires_at) {
            session = it->second;
        }
    }

    try {
        if (session.cookie.empty()) {
            // The direct bootstrap path is already proven by the runtime
            // diagnostic: it returns 200, issues _gtoken and allows /users.
            const auto bootstrap = http_.get(
                launch_url(kNookLinkBaseUrl, language, country),
                bootstrap_headers(web_service_token, language),
                10,
                4 * 1024 * 1024);
            if (bootstrap.status != 200) {
                log_nooklink_failure(
                    "bootstrap HTTP " + std::to_string(bootstrap.status));
                return {};
            }

            const auto gtoken = cookie_value(bootstrap, "_gtoken");
            if (gtoken.empty()) {
                log_nooklink_failure("bootstrap 200 but _gtoken is missing");
                return {};
            }
            session.source_token = web_service_token;
            // Preserve every Nintendo cookie from the bootstrap, not only
            // _gtoken. The working Worker carries a full CookieJar across each
            // request and later endpoints are allowed to depend on refreshed
            // session cookies.
            session.cookie = merge_cookie_header({}, bootstrap);
            if (session.cookie.empty()) session.cookie = "_gtoken=" + gtoken;
            session.expires_at = std::chrono::system_clock::now() + kSessionTtl;
        }

        const auto users_response = http_.get(
            std::string(kNookLinkBaseUrl) + "/api/sd/v1/users",
            nooklink_api_headers(session.cookie, language, country),
            10,
            4 * 1024 * 1024);
        if (users_response.status == 401 || users_response.status == 403) {
            log_nooklink_failure(
                "/users HTTP " + std::to_string(users_response.status));
            std::lock_guard lock(mutex_);
            sessions_.erase("nooklink");
            return {};
        }
        if (users_response.status != 200) {
            log_nooklink_failure(
                "/users HTTP " + std::to_string(users_response.status));
            return {};
        }
        session.cookie = merge_cookie_header(session.cookie, users_response);

        const auto users_json = Json::parse(users_response.text());
        const auto* users = users_json.find("users");
        if (!users || !users->is_array()) {
            log_nooklink_failure("/users response has no users array");
            return {};
        }
        if (users->as_array().empty()) {
            log_nooklink_failure("/users returned no linked NookLink residents");
            return {};
        }
        const auto& user = users->as_array().front();

        AnimalCrossingPresence presence;
        presence.resident_name = user.string("name");
        presence.image_uri = user.string("image");
        if (presence.image_uri.empty()) {
            presence.image_uri = user.string("image_url");
        }
        if (presence.image_uri.size() > 300) {
            // Discord currently rejects activity image URLs longer than 300
            // characters. Do not forward a user-specific/signed NookLink URL to
            // a third-party shortening service just to fit that presentation cap.
            presence.image_uri.clear();
        }
        session.user_id = user.string("id");
        std::string land_id;
        if (const auto* land = user.find("land"); land && land->is_object()) {
            presence.island_name = land->string("name");
            land_id = land->string("id");
        }
        // /users alone is sufficient for a truthful enriched RPC. Resident
        // bearer auth only adds deeper profile data/native fruit and must never
        // make the already-valid resident/island information disappear.
        presence.active = !presence.resident_name.empty() || !presence.island_name.empty();

        if (!session.user_id.empty() && session.auth_token.empty() &&
            !session.user_auth_attempted) {
            session.user_auth_attempted = true;
            const Json auth_body(Json::object{{"userId", session.user_id}});
            const auto auth_response = http_.post(
                std::string(kNookLinkBaseUrl) + "/api/sd/v1/auth_token",
                auth_body.dump(),
                nooklink_api_headers(session.cookie, language, country),
                "",
                10);
            session.cookie = merge_cookie_header(session.cookie, auth_response);

            if (auth_response.status / 100 == 2) {
                try {
                    const auto auth_json = Json::parse(auth_response.text());
                    session.auth_token = auth_json.string("token");
                    if (session.auth_token.empty()) {
                        log_nooklink_failure("/auth_token response has no token");
                    }
                } catch (...) {
                    log_nooklink_failure("/auth_token response is not valid JSON");
                }
            } else {
                log_nooklink_failure(
                    "/auth_token HTTP " + std::to_string(auth_response.status));
            }
        }

        if (!session.user_id.empty() && !session.auth_token.empty()) {
            auto profile_headers = nooklink_api_headers(
                session.cookie, language, country);
            profile_headers.push_back("Authorization: Bearer " + session.auth_token);

            // nxapi's authenticated NookLink user client deliberately uses
            // en-GB for profile endpoints even when the Nintendo Account locale
            // is not one of NookLink's supported API languages.
            constexpr char kNookLinkProfileLanguage[] = "en-GB";
            const auto user_profile_response = http_.get(
                std::string(kNookLinkBaseUrl) + "/api/sd/v1/users/" + session.user_id +
                    "/profile?language=" + kNookLinkProfileLanguage,
                profile_headers,
                10,
                4 * 1024 * 1024);
            session.cookie = merge_cookie_header(session.cookie, user_profile_response);

            bool user_auth_still_valid = true;
            if (user_profile_response.status == 401 || user_profile_response.status == 403) {
                log_nooklink_failure(
                    "resident profile HTTP " +
                    std::to_string(user_profile_response.status));
                session.auth_token.clear();
                user_auth_still_valid = false;
            } else if (user_profile_response.status == 200) {
                try {
                    const auto profile_json = Json::parse(user_profile_response.text());
                    const auto resident_name = profile_json.string("mPNm");
                    if (!resident_name.empty()) presence.resident_name = resident_name;
                    const auto island_name = profile_json.string("landName");
                    if (!island_name.empty()) presence.island_name = island_name;
                    const auto profile_image = profile_json.string("image");
                    if (!profile_image.empty()) {
                        presence.image_uri = profile_image;
                    } else {
                        const auto profile_image_url = profile_json.string("image_url");
                        if (!profile_image_url.empty()) {
                            presence.image_uri = profile_image_url;
                        }
                    }
                } catch (...) {
                    log_nooklink_failure("resident profile response is not valid JSON");
                }
            } else {
                log_nooklink_failure(
                    "resident profile HTTP " +
                    std::to_string(user_profile_response.status));
            }

            if (user_auth_still_valid && !land_id.empty()) {
                auto island_headers = nooklink_api_headers(
                    session.cookie, language, country);
                island_headers.push_back("Authorization: Bearer " + session.auth_token);
                const auto island_response = http_.get(
                    std::string(kNookLinkBaseUrl) + "/api/sd/v1/lands/" + land_id +
                        "/profile?language=" + kNookLinkProfileLanguage,
                    island_headers,
                    10,
                    4 * 1024 * 1024);
                session.cookie = merge_cookie_header(session.cookie, island_response);

                if (island_response.status == 401 || island_response.status == 403) {
                    log_nooklink_failure(
                        "island profile HTTP " +
                        std::to_string(island_response.status));
                    session.auth_token.clear();
                } else if (island_response.status == 200) {
                    try {
                        const auto island_json = Json::parse(island_response.text());
                        const auto island_name = island_json.string("mVNm");
                        if (!island_name.empty()) presence.island_name = island_name;
                        if (const auto* fruit = island_json.find("mFruit");
                            fruit && fruit->is_object()) {
                            presence.native_fruit = fruit->string("name");
                        }
                    } catch (...) {
                        log_nooklink_failure("island profile response is not valid JSON");
                    }
                } else {
                    log_nooklink_failure(
                        "island profile HTTP " +
                        std::to_string(island_response.status));
                }
            }
        }

        // The detailed profile may replace the initial /users image with a
        // longer signed URL, so enforce Discord's cap again before returning.
        if (presence.image_uri.size() > 300) {
            presence.image_uri.clear();
        }
        if (!presence.active) {
            log_nooklink_failure("/users returned no usable resident or island name");
        }
        {
            std::lock_guard lock(mutex_);
            if (language_ == language && country_ == country) {
                sessions_["nooklink"] = session;
            }
        }
        return presence;
    } catch (...) {
        log_nooklink_failure("exception while parsing NookLink response");
        std::lock_guard lock(mutex_);
        sessions_.erase("nooklink");
        return {};
    }
}

Splatoon2Presence GameServicesClient::fetch_splatoon2_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    ServiceSession session;
    std::string language;
    std::string country;
    {
        std::lock_guard lock(mutex_);
        language = language_;
        country = country_;
        const auto it = sessions_.find("splatnet2");
        if (it != sessions_.end() && it->second.source_token == web_service_token &&
            std::chrono::system_clock::now() < it->second.expires_at) {
            session = it->second;
        }
    }

    try {
        if (session.cookie.empty() || session.user_id.empty()) {
            const auto bootstrap = http_.get(
                launch_url(kSplatNet2BaseUrl, language, country),
                bootstrap_headers(web_service_token, language),
                10,
                8 * 1024 * 1024);
            if (bootstrap.status != 200) return {};
            const auto iksm = cookie_value(bootstrap, "iksm_session");
            const auto unique_id = html_attribute(bootstrap.text(), "data-unique-id");
            if (iksm.empty() || unique_id.empty()) return {};
            session.source_token = web_service_token;
            session.cookie = "iksm_session=" + iksm;
            session.user_id = unique_id;
            session.expires_at = std::chrono::system_clock::now() + kSessionTtl;
        }

        const std::vector<std::string> headers = {
            std::string("User-Agent: ") + kWebServiceUserAgent,
            "Cookie: " + session.cookie,
            "Accept: */*",
            "Accept-Language: en-GB,en-US;q=0.8",
            std::string("Referer: ") + kSplatNet2BaseUrl + "/home",
            "X-Requested-With: XMLHttpRequest",
            "X-Timezone-Offset: 0",
            "X-Unique-Id: " + session.user_id,
        };
        const auto response = http_.get(
            std::string(kSplatNet2BaseUrl) + "/api/records",
            headers,
            10,
            8 * 1024 * 1024);
        if (response.status != 200) {
            if (response.status == 401 || response.status == 403) {
                std::lock_guard lock(mutex_);
                sessions_.erase("splatnet2");
            }
            return {};
        }

        const auto json = Json::parse(response.text());
        const auto* records = json.find("records");
        if (!records || !records->is_object()) return {};
        const auto* player = records->find("player");
        if (!player || !player->is_object()) return {};

        Splatoon2Presence presence;
        presence.player_name = player->string("nickname");
        presence.player_level = player->integer("player_rank", 0);
        presence.star_rank = player->integer("star_rank", 0);
        if (const auto* weapon = player->find("weapon"); weapon && weapon->is_object()) {
            presence.weapon_name = weapon->string("name");
            presence.stage_image_uri = weapon->string("image");
        }

        std::vector<std::string> ranks;
        for (const auto& value : {
                 rank_value(*player, "udemae_zones", "Zones"),
                 rank_value(*player, "udemae_tower", "Tower"),
                 rank_value(*player, "udemae_rainmaker", "Rainmaker"),
                 rank_value(*player, "udemae_clam", "Clams")}) {
            if (!value.empty()) ranks.push_back(value);
        }
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            if (i) presence.rank_name += " / ";
            presence.rank_name += ranks[i];
        }
        presence.active = !presence.player_name.empty() ||
            !presence.weapon_name.empty() || presence.player_level > 0;
        {
            std::lock_guard lock(mutex_);
            if (language_ == language && country_ == country) {
                sessions_["splatnet2"] = session;
            }
        }
        return presence;
    } catch (...) {
        return {};
    }
}

}  // namespace nso
