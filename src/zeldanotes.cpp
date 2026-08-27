#include "nso_album_sync/zeldanotes.hpp"
#include "nso_album_sync/zeldanotes_regions.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/sse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nso {
namespace {

void log_zelda(const std::string& msg) {
    std::cerr << "[ZeldaNotes] " << msg << "\n";
    try {
        const auto path = std::filesystem::temp_directory_path() / "nso-album-sync-rpc.log";
        std::ofstream output(path, std::ios::app);
        if (output) output << "[ZeldaNotes] " << msg << '\n';
    } catch (...) {}
}

constexpr char kBaseUrl[] = "https://api.lp1.87abc152.srv.nintendo.net";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Linux; Android 10; Build/QP1A.190711.020; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/80.0.3987.162 Mobile Safari/537.36 com.nintendo.znca/3.4.1";
constexpr auto kSessionTtl = std::chrono::minutes(90);
constexpr char kPorterSessionAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr std::size_t kPorterSessionIdLength = 5;
constexpr std::size_t kMaxRouteHtmlBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxScriptBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxLocaleBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxScriptsToInspect = 64;
constexpr int kReconnectMaxSeconds = 30;
constexpr long kActionTimeoutSeconds = 5;

std::mutex g_bridge_mutex;
ZeldaNotesClient* g_client = nullptr;
ZeldaNotesRpcRefreshCallback g_rpc_refresh_callback;

struct MapPlace {
    std::int64_t uid = 0;
    std::string subcategory;
    std::string message_label;
    ZeldaNotesLayer layer = ZeldaNotesLayer::Ground;
    ZeldaNotesVector3 position;
};

struct WebMetadata {
    std::string start_action;
    std::string end_action;
    std::string ack_action;
    std::string deployment_id;
    std::string custom_avatar_url;
    std::map<std::string, std::string> labels;
    std::vector<MapPlace> places;

    bool protocol_ready() const {
        return !start_action.empty() && !end_action.empty() && !ack_action.empty();
    }
};

struct TowerRegion {
    const char* label;
    const char* localized_region_key;
    const char* english_region;
    double x;
    double z;
};

// These are the verified Zelda Notes tower/Skyview Tower coordinates from the
// captured Complete Guide dataset. They are intentionally tiny and stable: if
// Nintendo moves the lazy POI chunks in a future Next deployment, we can still
// turn live player coordinates into a coarse region instead of disabling the
// entire feature. Dynamic Complete Guide data still wins for precise POIs.
constexpr TowerRegion kTotkTowerRegions[] = {
    {"Ex_Tower01", "Ex_MapRegion_HyrulePrairie", "Central Hyrule", -298.85, -142.85},
    {"Ex_Tower02", "Ex_MapRegion_HyrulePrairie", "Central Hyrule", -1909.588, -1245.305},
    {"Ex_Tower03", "Ex_MapRegion_Hebura", "Hebra", -2311.495, -3062.495},
    {"Ex_Tower04", "Ex_MapRegion_Eldin", "Eldin", 1641.805, -1190.82},
    {"Ex_Tower05", "Ex_MapRegion_Tamul", "Akkala", 3499.0, -2026.0},
    {"Ex_Tower06", "Ex_MapRegion_Hateru", "Necluda", 1341.109, 1177.858},
    {"Ex_Tower07", "Ex_MapRegion_Lanayru", "Lanayru", 2866.062, -581.1915},
    {"Ex_Tower08", "Ex_MapRegion_HyrulePrairie", "Central Hyrule", -761.2766, 1019.228},
    {"Ex_Tower09", "Ex_MapRegion_Gerudo", "Gerudo", -2438.851, 2182.764},
    {"Ex_Tower10", "Ex_MapRegion_Gerudo", "Gerudo", -3960.877, 1305.596},
    {"Ex_Tower11", "Ex_MapRegion_Hateru", "Necluda", 2420.0, 2754.891},
    {"Ex_Tower12", "Ex_MapRegion_HyrulePrairie", "Central Hyrule", 343.6745, -3141.648},
    {"Ex_Tower13", "Ex_MapRegion_Firone", "Faron", 604.8388, 2126.876},
    {"Ex_Tower14", "Ex_MapRegion_Lanayru", "Lanayru", 3847.638, 1314.911},
    {"Ex_Tower15", "Ex_MapRegion_Hebura", "Hebra", -3679.585, -2346.404},
};

constexpr TowerRegion kBotwTowerRegions[] = {
    {"U_Tower01", "U_MapRegion_Hebura", "Hebra", -2173.0, -2034.0},
    {"U_Tower02", "U_MapRegion_Hebura", "Hebra", -3613.748, -990.1647},
    {"U_Tower03", "U_MapRegion_Gerudo", "Gerudo", -3666.0, 1828.6},
    {"U_Tower04", "U_MapRegion_Gerudo", "Gerudo", -2306.836, 2437.32},
    {"U_Tower05", "U_MapRegion_HyrulePrairie ", "Central Hyrule", 883.8843, -1605.71},
    {"U_Tower06", "U_MapRegion_HyrulePrairie ", "Central Hyrule", -788.645, 442.0306},
    {"U_Tower07", "U_MapRegion_HyrulePrairie ", "Central Hyrule", -560.0352, 1694.863},
    {"U_Tower08", "U_MapRegion_Hateru", "Necluda", 1016.777, 1714.082},
    {"U_Tower09", "U_MapRegion_Firone", "Faron", -31.81555, 2961.601},
    {"U_Tower10", "U_MapRegion_Eldin", "Eldin", 2174.151, -1556.781},
    {"U_Tower11", "U_MapRegion_Tamul", "Akkala", 3308.0, -1500.1},
    {"U_Tower12", "U_MapRegion_Lanayru", "Lanayru", 2258.0, -109.0},
    {"U_Tower13", "U_MapRegion_Hateru", "Necluda", 2735.5, 2133.5},
    {"U_Tower14", "U_MapRegion_Firone", "Faron", 1331.203, 3273.723},
    {"U_Tower15", "U_MapRegion_HyrulePrairie ", "Central Hyrule", -1755.3, -774.3},
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim_copy(std::string value) {
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

bool contains_any(const std::string& value, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

ZeldaNotesGame game_from_presence(
    const std::string& title_id,
    const std::string& game_name) {
    const auto by_id = zelda_notes_game_for_title_id(title_id);
    if (by_id != ZeldaNotesGame::Unknown) return by_id;
    if (contains_any(game_name, {
            "Breath of the Wild", "ブレス オブ ザ ワイルド"})) {
        return ZeldaNotesGame::BreathOfTheWild;
    }
    if (contains_any(game_name, {
            "Tears of the Kingdom", "ティアーズ オブ ザ キングダム"})) {
        return ZeldaNotesGame::TearsOfTheKingdom;
    }
    return ZeldaNotesGame::Unknown;
}

std::string header_value(const HttpResponse& response, const std::string& key) {
    const auto it = response.headers.find(lower(key));
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
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string session_cookie(const HttpResponse& response) {
    for (const auto& line : set_cookie_lines(response)) {
        std::size_t start = 0;
        while (start < line.size() &&
               std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        const auto eq = line.find('=', start);
        if (eq == std::string::npos) continue;
        const auto name = line.substr(start, eq - start);
        const auto lower_name = lower(name);
        if (lower_name != "a5_token" &&
            lower_name.find("session") == std::string::npos) {
            continue;
        }
        const auto value_start = eq + 1;
        auto end = line.find(';', value_start);
        if (end == std::string::npos) end = line.size();
        return name + "=" + line.substr(value_start, end - value_start);
    }
    return {};
}

std::vector<std::string> bootstrap_headers(
    const std::string& token,
    const std::string& language,
    const std::string& country) {
    return {
        "Upgrade-Insecure-Requests: 1",
        std::string("User-Agent: ") + kUserAgent,
        "x-appplatform: android",
        "x-appcolorscheme: DARK",
        "x-gamewebtoken: " + token,
        "dnt: 1",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: " + language,
        "X-NACountry: " + country,
        "X-Requested-With: com.nintendo.znca",
    };
}

std::string cookie_header(const std::string& session, const std::string& language) {
    return "Cookie: " + session + "; lang=" + language;
}

std::vector<std::string> authenticated_headers(
    const std::string& session,
    const std::string& language,
    const std::string& country,
    const std::string& accept) {
    return {
        std::string("User-Agent: ") + kUserAgent,
        "Accept: " + accept,
        "Accept-Language: " + language,
        "X-NACountry: " + country,
        cookie_header(session, language),
        "dnt: 1",
    };
}

bool read_vector3(
    const Json& message,
    const std::string& key,
    ZeldaNotesVector3& output) {
    const auto* value = message.find(key);
    if (value == nullptr || !value->is_array()) return false;
    const auto& coordinates = value->as_array();
    if (coordinates.size() < 3 ||
        !coordinates[0].is_number() ||
        !coordinates[1].is_number() ||
        !coordinates[2].is_number()) {
        return false;
    }

    const auto x = coordinates[0].as_number();
    const auto y = coordinates[1].as_number();
    const auto z = coordinates[2].as_number();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }

    output = ZeldaNotesVector3{x, y, z};
    return true;
}

ZeldaNotesLiveMessageType live_message_type(const std::string& type) {
    if (type == "open") return ZeldaNotesLiveMessageType::Open;
    if (type == "map_sync_start_ack") {
        return ZeldaNotesLiveMessageType::MapSyncStartAck;
    }
    if (type == "map_sync_player_info") {
        return ZeldaNotesLiveMessageType::MapSyncPlayerInfo;
    }
    return ZeldaNotesLiveMessageType::Unknown;
}

std::string route_url(ZeldaNotesGame game) {
    const auto* short_name = zelda_notes_short_name(game);
    if (short_name == nullptr || *short_name == '\0') return {};
    return std::string(kBaseUrl) + "/" + short_name + "/complete-guide";
}

std::string percent_decode(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const char hex[3] = {value[i + 1], value[i + 2], '\0'};
            char* end = nullptr;
            const auto byte_val = std::strtoul(hex, &end, 16);
            if (end == hex + 2) {
                output += static_cast<char>(byte_val);
                i += 2;
                continue;
            }
        }
        output += value[i];
    }
    return output;
}

std::string percent_encode(const std::string& value) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        const bool unreserved =
            std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            output << static_cast<char>(c);
        } else {
            output << '%' << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(c);
        }
    }
    return output.str();
}

std::string router_state_tree(ZeldaNotesGame game) {
    const std::string short_name = zelda_notes_short_name(game);
    const std::string raw =
        "[\"\",{\"children\":[\"" + short_name +
        "\",{\"children\":[\"complete-guide\",{\"children\":[\"__PAGE__\",{},null,null]},null,null]},null,null,true]},null,null,true]";
    return percent_encode(raw);
}

bool is_hex_action_id(const std::string& value) {
    if (value.size() != 40) return false;
    for (const unsigned char c : value) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::string find_server_reference(
    const std::string& script,
    const std::string& action_name) {
    std::size_t search_from = 0;
    while (true) {
        const auto name_at = script.find(action_name, search_from);
        if (name_at == std::string::npos) break;
        const auto begin = name_at > 512 ? name_at - 512 : 0;
        std::string best;
        for (auto quote = script.find('"', begin);
             quote != std::string::npos && quote < name_at;
             quote = script.find('"', quote + 1)) {
            const auto end = script.find('"', quote + 1);
            if (end == std::string::npos || end > name_at) break;
            const auto candidate = script.substr(quote + 1, end - quote - 1);
            if (is_hex_action_id(candidate)) best = candidate;
            quote = end;
        }
        if (!best.empty()) return best;
        search_from = name_at + action_name.size();
    }
    return {};
}

std::string html_unescape(std::string value) {
    std::size_t at = 0;
    while ((at = value.find("&amp;", at)) != std::string::npos) {
        value.replace(at, 5, "&");
        ++at;
    }
    return value;
}

std::vector<std::string> extract_script_urls(const std::string& html) {
    std::vector<std::string> urls;
    std::size_t position = 0;
    while ((position = html.find("src=", position)) != std::string::npos) {
        position += 4;
        while (position < html.size() &&
               std::isspace(static_cast<unsigned char>(html[position]))) {
            ++position;
        }
        if (position >= html.size() ||
            (html[position] != '"' && html[position] != '\'')) {
            continue;
        }
        const char quote = html[position++];
        const auto end = html.find(quote, position);
        if (end == std::string::npos) break;
        auto url = html_unescape(html.substr(position, end - position));
        position = end + 1;
        if (url.find(".js") == std::string::npos ||
            url.find("/_next/") == std::string::npos) {
            continue;
        }
        if (url.rfind("/", 0) == 0) {
            url = std::string(kBaseUrl) + url;
        } else if (url.rfind(kBaseUrl, 0) != 0) {
            continue;
        }
        if (std::find(urls.begin(), urls.end(), url) == urls.end()) {
            urls.push_back(std::move(url));
        }
        if (urls.size() >= kMaxScriptsToInspect) break;
    }
    return urls;
}

std::string deployment_id_from_url(const std::string& url) {
    const auto marker = url.find("dpl=");
    if (marker == std::string::npos) return {};
    const auto start = marker + 4;
    auto end = url.find('&', start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
}

std::string decode_js_single_quoted(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c != '\\' || i + 1 >= value.size()) {
            decoded.push_back(c);
            continue;
        }
        const char next = value[++i];
        switch (next) {
            case '\\': decoded.push_back('\\'); break;
            case '\'': decoded.push_back('\''); break;
            case '"': decoded.push_back('"'); break;
            case '/': decoded.push_back('/'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            default:
                // Preserve JSON escapes such as \uXXXX for Json::parse().
                decoded.push_back('\\');
                decoded.push_back(next);
                break;
        }
    }
    return decoded;
}

bool wanted_place_subcategory(ZeldaNotesGame game, const std::string& subcategory) {
    if (game == ZeldaNotesGame::TearsOfTheKingdom) {
        return subcategory == "village" ||
            subcategory == "stable" ||
            subcategory == "structure" ||
            subcategory == "skyviewTower" ||
            subcategory == "other" ||
            subcategory == "shrine" ||
            subcategory == "lightroot";
    }
    return subcategory == "village" ||
        subcategory == "hatago" ||
        subcategory == "structure" ||
        subcategory == "tower" ||
        subcategory == "other" ||
        subcategory == "dungeon";
}

bool parse_map_array(
    const std::string& json_text,
    ZeldaNotesGame wanted_game,
    std::vector<MapPlace>& output) {
    try {
        const auto root = Json::parse(json_text);
        if (!root.is_array() || root.as_array().empty()) return false;

        bool looks_totk = false;
        bool looks_like_map = false;
        for (const auto& item : root.as_array()) {
            if (!item.is_object()) continue;
            if (item.find("uid") != nullptr &&
                item.find("viewCategory") != nullptr &&
                item.find("coordinates") != nullptr) {
                looks_like_map = true;
            }
            if (item.find("layer") != nullptr) looks_totk = true;
            if (looks_like_map && looks_totk) break;
        }
        if (!looks_like_map) return false;
        if ((wanted_game == ZeldaNotesGame::TearsOfTheKingdom) != looks_totk) {
            return false;
        }

        std::vector<MapPlace> places;
        for (const auto& item : root.as_array()) {
            if (!item.is_object() || item.string("viewCategory") != "Location") continue;
            const auto subcategory = item.string("viewSubCategory");
            if (!wanted_place_subcategory(wanted_game, subcategory)) continue;
            ZeldaNotesVector3 position;
            if (!read_vector3(item, "coordinates", position)) continue;

            MapPlace place;
            place.uid = item.integer("uid");
            place.subcategory = subcategory;
            place.message_label = item.string("messageLabel");
            place.position = position;
            if (wanted_game == ZeldaNotesGame::TearsOfTheKingdom) {
                place.layer = zelda_notes_layer_from_wire(item.string("layer"));
                if (place.layer == ZeldaNotesLayer::Unknown) continue;
            } else {
                place.layer = ZeldaNotesLayer::Ground;
            }
            if (place.uid == 0 || place.message_label.empty()) continue;
            places.push_back(std::move(place));
        }
        if (places.empty()) return false;
        output = std::move(places);
        return true;
    } catch (...) {
        return false;
    }
}

bool extract_map_dataset(
    const std::string& script,
    ZeldaNotesGame game,
    std::vector<MapPlace>& output) {
    const std::string marker = "JSON.parse('";
    std::size_t position = 0;
    while ((position = script.find(marker, position)) != std::string::npos) {
        const auto start = position + marker.size();
        std::size_t end = start;
        bool escaped = false;
        for (; end < script.size(); ++end) {
            const char c = script[end];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '\'') break;
        }
        if (end >= script.size()) break;
        const auto raw = script.substr(start, end - start);
        const auto decoded = decode_js_single_quoted(raw);
        if (decoded.rfind("[{\"uid\":", 0) == 0 &&
            parse_map_array(decoded, game, output)) {
            return true;
        }
        position = end + 1;
    }
    return false;
}

std::map<std::string, std::string> parse_labels(const std::string& text) {
    std::map<std::string, std::string> labels;
    try {
        const auto root = Json::parse(text);
        if (!root.is_object()) return labels;
        for (const auto& [key, value] : root.as_object()) {
            if (value.is_string()) labels[key] = trim_copy(value.as_string());
        }
    } catch (...) {
    }
    return labels;
}

std::map<std::string, std::string> fetch_labels(
    HttpClient& http,
    const std::string& session,
    const std::string& language,
    const std::string& country) {
    const auto fetch = [&](const std::string& locale) {
        const auto response = http.get(
            std::string(kBaseUrl) + "/common/locales/" + locale + "/complete_guide.json",
            authenticated_headers(session, locale, country, "application/json,*/*"),
            10,
            kMaxLocaleBytes);
        if (response.status / 100 != 2) return std::map<std::string, std::string>{};
        return parse_labels(response.text());
    };

    auto labels = fetch(language);
    if (labels.empty() && language != "en-GB") labels = fetch("en-GB");
    return labels;
}

WebMetadata discover_web_metadata(
    HttpClient& http,
    ZeldaNotesGame game,
    const std::string& session,
    const std::string& language,
    const std::string& country) {
    WebMetadata metadata;
    const auto page_url = route_url(game);
    if (page_url.empty()) return metadata;

    log_zelda("discover_web_metadata: requesting page " + page_url);
    const auto page = http.get(
        page_url,
        authenticated_headers(
            session, language, country,
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
        12,
        kMaxRouteHtmlBytes);
    log_zelda("discover_web_metadata: page status=" + std::to_string(page.status));
    const auto html = page.text();
    metadata.deployment_id = header_value(page, "x-deployment-id");
    const auto scripts = extract_script_urls(html);
    for (const auto& script_url : scripts) {
        if (metadata.deployment_id.empty()) {
            metadata.deployment_id = deployment_id_from_url(script_url);
        }
    }
    if (metadata.deployment_id.empty()) {
        metadata.deployment_id = "783666f6880ab3979bdc7b15f8ad24f544e472e5";
    }

    metadata.start_action = "70133dd2eb7d5126fda8aa9c8ff56d5a0376deadba";
    metadata.end_action = "70133dd2eb7d5126fda8aa9c8ff56d5a0376deadba";
    metadata.ack_action = "400d043452ef637b91e45e5861062b9677aa6fbf22";
    log_zelda("discover_web_metadata final: start=" + metadata.start_action + " ack=" + metadata.ack_action + " dpl=" + metadata.deployment_id);

    // Look for custom UGC avatar in complete-guide HTML
    auto find_ugc_avatar = [&](const std::string& source) -> std::string {
        const auto marker = source.find("storage.googleapis.com");
        if (marker == std::string::npos) return {};
        auto start = source.rfind("url=", marker);
        if (start != std::string::npos) {
            start += 4;
            auto end = source.find_first_of("&\"' ", start);
            if (end != std::string::npos) {
                return percent_decode(html_unescape(source.substr(start, end - start)));
            }
        }
        auto start_direct = source.rfind("https://storage.googleapis.com", marker);
        if (start_direct != std::string::npos) {
            auto end_direct = source.find_first_of("\"' ", start_direct);
            if (end_direct != std::string::npos) {
                return html_unescape(source.substr(start_direct, end_direct - start_direct));
            }
        }
        return {};
    };

    metadata.custom_avatar_url = find_ugc_avatar(html);

    // If not found in complete-guide, check profile page
    if (metadata.custom_avatar_url.empty()) {
        const auto profile_url = std::string(kBaseUrl) + "/" + zelda_notes_short_name(game) + "/profile";
        const auto profile_page = http.get(
            profile_url,
            authenticated_headers(
                session, language, country,
                "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
            8,
            kMaxRouteHtmlBytes);
        if (profile_page.status == 200) {
            metadata.custom_avatar_url = find_ugc_avatar(profile_page.text());
        }
    }
    if (!metadata.custom_avatar_url.empty()) {
        log_zelda("discover_web_metadata: active custom avatar=" + metadata.custom_avatar_url);
    }

    try {
        metadata.labels = fetch_labels(http, session, language, country);
    } catch (...) {
        // Region resolver has verified English fallbacks. Missing localization
        // should reduce precision, never disable a healthy live map stream.
    }
    return metadata;
}

bool action_succeeded(const HttpResponse& response) {
    if (response.status / 100 != 2) return false;
    const auto text = response.text();
    return text.empty() || text.find("\"isSuccess\":true") != std::string::npos || text.find("isSuccess") == std::string::npos;
}

bool send_server_action(
    HttpClient& http,
    const WebMetadata& metadata,
    ZeldaNotesGame game,
    const std::string& session,
    const std::string& language,
    const std::string& country,
    const std::string& action,
    const Json::array& arguments) {
    if (action.empty()) return false;
    const auto page_url = route_url(game);
    auto headers = authenticated_headers(
        session, language, country, "text/x-component");
    headers.push_back("Next-Action: " + action);
    headers.push_back("Next-Router-State-Tree: " + router_state_tree(game));
    headers.push_back(std::string("Origin: ") + kBaseUrl);
    headers.push_back("Referer: " + page_url);
    if (!metadata.deployment_id.empty()) {
        headers.push_back("X-Deployment-Id: " + metadata.deployment_id);
    }

    const auto body = Json(arguments).dump();
    log_zelda("send_server_action POST " + page_url + " action=" + action + " body=" + body);
    const auto response = http.post(
        page_url,
        body,
        headers,
        "text/plain;charset=UTF-8",
        kActionTimeoutSeconds);
    const bool success = action_succeeded(response);
    log_zelda("send_server_action status=" + std::to_string(response.status) + " success=" + (success ? "true" : "false") + " resp=" + response.text());
    return success;
}

double horizontal_distance(
    const ZeldaNotesVector3& a,
    const ZeldaNotesVector3& b) {
    const auto dx = a.x - b.x;
    const auto dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

std::string localized_label(
    const WebMetadata& metadata,
    const std::string& key) {
    const auto it = metadata.labels.find(key);
    if (it == metadata.labels.end()) return {};
    return trim_copy(it->second);
}

std::string resolve_region(
    const WebMetadata& metadata,
    ZeldaNotesGame game,
    const ZeldaNotesVector3& position) {
    const TowerRegion* towers = game == ZeldaNotesGame::TearsOfTheKingdom
        ? kTotkTowerRegions
        : kBotwTowerRegions;
    const std::size_t tower_count = game == ZeldaNotesGame::TearsOfTheKingdom
        ? std::size(kTotkTowerRegions)
        : std::size(kBotwTowerRegions);

    const TowerRegion* nearest = nullptr;
    double nearest_distance = (std::numeric_limits<double>::max)();
    for (std::size_t i = 0; i < tower_count; ++i) {
        ZeldaNotesVector3 tower_position{towers[i].x, 0.0, towers[i].z};
        const auto distance = horizontal_distance(position, tower_position);
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = &towers[i];
        }
    }
    if (nearest == nullptr) return {};
    const auto localized = localized_label(metadata, nearest->localized_region_key);
    return localized.empty() ? nearest->english_region : localized;
}

struct DistanceThresholds {
    double at = 80.0;
    double nearby = 300.0;
};

DistanceThresholds thresholds_for(const std::string& subcategory) {
    if (subcategory == "village") return {180.0, 650.0};
    if (subcategory == "stable" || subcategory == "hatago") return {120.0, 450.0};
    if (subcategory == "structure") return {110.0, 500.0};
    if (subcategory == "other") return {100.0, 480.0};
    if (subcategory == "skyviewTower" || subcategory == "tower") {
        return {90.0, 350.0};
    }
    if (subcategory == "shrine" || subcategory == "dungeon" ||
        subcategory == "lightroot") {
        return {65.0, 260.0};
    }
    return {70.0, 250.0};
}

int category_priority(const std::string& subcategory) {
    if (subcategory == "village") return 100;
    if (subcategory == "stable" || subcategory == "hatago") return 95;
    if (subcategory == "structure") return 90;
    if (subcategory == "other") return 85;
    if (subcategory == "skyviewTower" || subcategory == "tower") return 75;
    if (subcategory == "shrine" || subcategory == "dungeon" ||
        subcategory == "lightroot") return 60;
    return 40;
}

bool same_layer(const MapPlace& place, const ZeldaNotesLiveState& state) {
    if (state.game == ZeldaNotesGame::BreathOfTheWild) return true;
    return place.layer == state.layer;
}

ZeldaNotesResolvedLocation resolve_location(
    const WebMetadata& metadata,
    const ZeldaNotesLiveState& state,
    const ZeldaNotesResolvedLocation& previous) {
    ZeldaNotesResolvedLocation result;
    result.layer = state.layer;
    result.region = resolve_region(metadata, state.game, state.position);

    // Check exact game-derived 3D collision volumes (villages, stables, towers, sky archipelagos, depths mines)
    if (state.game == ZeldaNotesGame::TearsOfTheKingdom) {
        const auto loc_3d = resolve_totk_location_3d(state.position, state.layer);
        if (loc_3d.matched) {
            result.poi = loc_3d.name;
            result.stage_image_uri = loc_3d.image_url;
            result.at_poi = true;
            result.valid = true;
            return result;
        }
    } else if (state.game == ZeldaNotesGame::BreathOfTheWild) {
        const auto loc_3d = resolve_botw_location_3d(state.position);
        if (loc_3d.matched) {
            result.poi = loc_3d.name;
            result.stage_image_uri = loc_3d.image_url;
            result.at_poi = true;
            result.valid = true;
            return result;
        }
    }

    struct Candidate {
        const MapPlace* place = nullptr;
        double distance = 0.0;
        double score = 0.0;
    };
    Candidate best;
    best.score = (std::numeric_limits<double>::max)();

    for (const auto& place : metadata.places) {
        if (!same_layer(place, state)) continue;
        const auto name = localized_label(metadata, place.message_label);
        if (name.empty() || name == "???") continue;
        const auto thresholds = thresholds_for(place.subcategory);
        const auto distance = horizontal_distance(state.position, place.position);
        if (distance > thresholds.nearby) continue;
        const auto priority_penalty =
            static_cast<double>(100 - category_priority(place.subcategory)) * 0.004;
        const auto score = distance / thresholds.nearby + priority_penalty;
        if (score < best.score) best = Candidate{&place, distance, score};
    }

    if (previous.poi_uid != 0 && best.place != nullptr &&
        best.place->uid != previous.poi_uid) {
        for (const auto& place : metadata.places) {
            if (place.uid != previous.poi_uid || !same_layer(place, state)) continue;
            const auto thresholds = thresholds_for(place.subcategory);
            const auto distance = horizontal_distance(state.position, place.position);
            if (distance <= thresholds.nearby * 1.15 &&
                best.distance > distance * 0.75) {
                best = Candidate{&place, distance, 0.0};
            }
            break;
        }
    }

    if (best.place != nullptr) {
        result.poi = localized_label(metadata, best.place->message_label);
        result.poi_uid = best.place->uid;
        result.poi_distance = best.distance;
        result.subcategory = best.place->subcategory;
        result.stage_image_uri = resolve_poi_artwork(result.poi, state.game);
        const auto thresholds = thresholds_for(best.place->subcategory);
        result.at_poi = best.distance <= thresholds.at;
        result.near_poi = !result.at_poi;
    }

    result.valid = !result.region.empty() || !result.poi.empty();
    return result;
}

std::string clamp_activity_text(std::string value) {
    constexpr std::size_t kMaxBytes = 128;
    if (value.size() <= kMaxBytes) return value;
    value.resize(kMaxBytes - 3);
    while (!value.empty() &&
           (static_cast<unsigned char>(value.back()) & 0xC0U) == 0x80U) {
        value.pop_back();
    }
    value += "...";
    return value;
}

std::string generate_zelda_lore_activity(
    const ZeldaNotesLiveState& state,
    const ZeldaNotesResolvedLocation& location) {
    const auto& poi = location.poi;
    const auto& region = location.region;
    const auto& subcategory = location.subcategory;
    const double x = state.position.x;
    const double y = state.position.y;
    const double z = state.position.z;

    // 1. Landmark & Specific POI Context
    if (location.at_poi || location.near_poi) {
        if (poi.find("Hyrule Castle") != std::string::npos || poi.find("Sanctum") != std::string::npos) {
            return "Infiltrating Hyrule Castle";
        }
        if (poi.find("Temple of Time") != std::string::npos) {
            return "Standing at the Temple of Time";
        }
        if (poi.find("Forgotten Temple") != std::string::npos) {
            return "Exploring the Forgotten Temple";
        }
        if (poi.find("Yiga") != std::string::npos || poi.find("Hideout") != std::string::npos) {
            return "Infiltrating Yiga Clan Territory";
        }
        if (poi.find("Coliseum") != std::string::npos || poi.find("Colosseum") != std::string::npos) {
            return "Challenging Ancient Arenas";
        }
        if (poi.find("Citadel") != std::string::npos) {
            return "Exploring Ancient Citadel Ruins";
        }
        if (poi.find("Labyrinth") != std::string::npos || poi.find("Maze") != std::string::npos) {
            return "Navigating Ancient Labyrinths";
        }
        if (poi.find("Chasm") != std::string::npos) {
            return "Descending into the Chasm";
        }
        if (poi.find("Cave") != std::string::npos || poi.find("Well") != std::string::npos ||
            poi.find("Grotto") != std::string::npos) {
            return "Exploring Caverns & Tunnels";
        }
        if (poi.find("Fairy") != std::string::npos) {
            return "Visiting the Great Fairy";
        }
        if (poi.find("Lab") != std::string::npos) {
            return "Visiting the Ancient Tech Lab";
        }
        if (poi.find("Stable") != std::string::npos || subcategory == "stable" || subcategory == "hatago") {
            return "Resting at " + poi;
        }
        if (poi.find("Tower") != std::string::npos || subcategory == "skyviewTower" || subcategory == "tower") {
            return "Surveying from " + poi;
        }
        if (poi.find("Shrine") != std::string::npos || subcategory == "shrine") {
            return "Investigating Shrine of Light";
        }
        if (poi.find("Lightroot") != std::string::npos || subcategory == "lightroot") {
            return "Resting by a Lightroot";
        }
        if (poi.find("Temple") != std::string::npos || poi.find("Mine") != std::string::npos ||
            poi.find("Forge") != std::string::npos || subcategory == "dungeon") {
            return "Delving into " + poi;
        }
        if (poi.find("Archipelago") != std::string::npos || poi.find("Island") != std::string::npos) {
            if (state.layer == ZeldaNotesLayer::Sky) {
                return "Exploring " + poi;
            }
        }
        if (subcategory == "village" || poi.find("Town") != std::string::npos ||
            poi.find("Village") != std::string::npos || poi.find("Landing") != std::string::npos ||
            poi.find("Domain") != std::string::npos || poi.find("City") != std::string::npos) {
            return location.at_poi ? "Visiting " + poi : "Approaching " + poi;
        }
    }

    // 2. Sky Layer Sub-Regions
    if (state.game == ZeldaNotesGame::TearsOfTheKingdom && state.layer == ZeldaNotesLayer::Sky) {
        if (y > 2200.0) return "Soaring in the Upper Stratosphere";
        if (x > 3000.0 && z < -1000.0) return "Navigating the Sokkala Sky Islands";
        if (x < -2500.0 && z < -1500.0) return "Navigating the Hebra Sky Realm";
        if (x < -2500.0 && z > 1500.0) return "Navigating the Gerudo Sky Realm";
        if (x > 1500.0 && z < -1500.0) return "Navigating the Eldin Sky Realm";
        if (x > 1500.0 && z > 1500.0) return "Navigating the Necluda Sky Realm";
        if (x > -500.0 && x < 1000.0 && z > 500.0 && z < 2000.0) return "Exploring the Great Sky Island";
        return !region.empty() ? "Soaring above " + region : "Navigating the Sky Archipelagos";
    }

    // 3. Depths Layer Sub-Regions
    if (state.game == ZeldaNotesGame::TearsOfTheKingdom && state.layer == ZeldaNotesLayer::Underground) {
        if (y < -800.0) return "Trekking the Abyssal Depths";
        if (x > 2500.0 && z < -1000.0) return "Trekking the Depths of Akkala";
        if (x > 1000.0 && z < -2000.0) return "Navigating the Volcanic Eldin Depths";
        if (x < -2000.0 && z > 1000.0) return "Trekking the Gerudo Desert Depths";
        if (x < -2000.0 && z < -1000.0) return "Braving the Freezing Hebra Depths";
        if (x > 1500.0 && z > -500.0 && z < 1000.0) return "Navigating the Lanayru Depths";
        if (x > -1500.0 && x < 1500.0 && z > -1500.0 && z < 1500.0) return "Trekking Central Hyrule Depths";
        return !region.empty() ? "Surveying the Depths below " + region : "Trekking the Lightless Depths";
    }

    // 4. Surface Granular Sub-Regions
    if (region == "Akkala") {
        if (x > 4000.0 && z < -2000.0) return "Exploring the Rist Peninsula Coast";
        if (z < -2800.0) return "Wandering Deep Akkala";
        if (z < -2200.0 && x < 3600.0) return "Exploring near Skull Lake";
        if (x > 3200.0 && z > -2000.0 && z < -1200.0) return "Wandering around Lake Akkala";
        if (z > -1200.0) return "Traversing South Akkala Plains";
        return "Wandering the Akkala Highlands";
    }

    if (region == "Central Hyrule" || region == "Hyrule Field") {
        if (z < -1000.0 && x > -500.0 && x < 500.0) return "Surveying Hyrule Castle Town Ruins";
        if (x < -1000.0) return "Roaming Western Hyrule Plains";
        if (x > 1000.0) return "Wandering near Crenel Hills";
        if (z > 500.0) return "Traversing Central Hyrule Plains";
        return "Roaming the Heart of Hyrule Field";
    }

    if (region == "Eldin" || region == "Death Mountain") {
        if (x > 2000.0 && z < -2500.0) return "Scaling the Summit of Death Mountain";
        if (z < -3000.0) return "Climbing the Northern Eldin Peaks";
        if (x < 1500.0) return "Braving the Crags of Eldin Canyon";
        return "Traversing the Scorching Lava Beds";
    }

    if (region == "Hebra" || region == "Tabantha") {
        if (x < -2500.0 && z < -2500.0) return "Braving the Summit of Mount Hebra";
        if (x > -2500.0 && z < -2500.0) return "Traversing Tabantha Tundra Snowfields";
        if (z > -2000.0) return "Wandering the Tabantha Frontier";
        return "Braving the Freezing Hebra Peaks";
    }

    if (region == "Gerudo") {
        if (z > 2500.0 && x < -2500.0) return "Traversing the Great Desert Dunes";
        if (z < 1500.0) return "Scaling the Frozen Gerudo Highlands";
        if (x > -2500.0) return "Navigating the Narrow Gerudo Canyons";
        return "Traversing the Shifting Sands";
    }

    if (region == "Lanayru") {
        if (x > 3000.0 && z > 500.0) return "Braving Mount Lanayru Snowfields";
        if (x < 2000.0 && z > -500.0) return "Navigating the Lanayru Wetlands";
        if (x > 2500.0 && z < -500.0) return "Roaming near Zora's Domain";
        return "Roaming the Rushing Waters of Lanayru";
    }

    if (region == "Necluda" || region == "Dueling Peaks") {
        if (x < 2000.0) return "Traversing the Cleft of Dueling Peaks";
        if (x > 3000.0) return "Roaming the Valleys of East Necluda";
        return "Wandering Peaceful Necluda";
    }

    if (region == "Faron" || region == "Lake Hylia") {
        if (x < 500.0 && z > 2000.0) return "Roaming the Shores of Lake Hylia";
        if (x > 2500.0) return "Wandering the Sunny Palmorae Coast";
        return "Venturing through the Dense Faron Jungle";
    }

    if (region == "Great Hyrule Forest") {
        return "Navigating the Mystical Lost Woods";
    }

    if (region == "Hyrule Ridge") {
        return "Traversing the Windy Hyrule Ridge";
    }

    if (region == "Great Plateau") {
        return "Exploring the Great Plateau";
    }

    return !region.empty() ? "Exploring the Realm of " + region : "Roaming the Lands of Hyrule";
}

ZeldaNotesPresence format_rpc_presence(
    const WebMetadata& metadata,
    const ZeldaNotesLiveState& state,
    const ZeldaNotesResolvedLocation& location) {
    ZeldaNotesPresence presence;
    if (!location.valid) return presence;

    // Line 1 (Details): In-Game Location
    std::string details;
    if (!location.poi.empty() && location.at_poi) {
        details = "At " + location.poi;
    } else if (!location.poi.empty()) {
        details = "Near " + location.poi;
    } else if (!location.region.empty()) {
        details = "Exploring " + location.region;
    } else {
        details = "Exploring Hyrule";
    }

    // Line 2 (State): Atmospheric Lore & In-Game Activity
    const std::string secondary = generate_zelda_lore_activity(state, location);

    if (details.empty() || secondary.empty()) return {};
    presence.profile_summary = clamp_activity_text(details);
    presence.title_name = clamp_activity_text(secondary);
    presence.stage_image_uri = !location.stage_image_uri.empty()
        ? location.stage_image_uri
        : resolve_zelda_region_artwork(location.region, state.game, state.layer);
    presence.stage_name = !location.poi.empty()
        ? location.poi
        : (!location.region.empty() ? location.region : "Hyrule");
    presence.active = true;
    return presence;
}

void notify_rpc_refresh() {
    ZeldaNotesRpcRefreshCallback callback;
    {
        std::lock_guard lock(g_bridge_mutex);
        callback = g_rpc_refresh_callback;
    }
    if (callback) callback();
}

int next_backoff(int current) {
    return std::min(kReconnectMaxSeconds, std::max(2, current * 2));
}

}  // namespace

ZeldaNotesLayer zelda_notes_layer_from_wire(const std::string& layer) {
    if (layer == "Ground") return ZeldaNotesLayer::Ground;
    if (layer == "Sky") return ZeldaNotesLayer::Sky;
    if (layer == "Underground") return ZeldaNotesLayer::Underground;
    return ZeldaNotesLayer::Unknown;
}

std::string zelda_notes_generate_porter_session_id() {
    static_assert(
        sizeof(kPorterSessionAlphabet) - 1 == 62,
        "Zelda Notes porter-session alphabet must stay alphanumeric");

    static thread_local std::mt19937_64 generator([] {
        std::random_device random;
        std::seed_seq seed{
            random(), random(), random(), random(),
            random(), random(), random(), random(),
        };
        return std::mt19937_64(seed);
    }());
    std::uniform_int_distribution<std::size_t> distribution(
        0, sizeof(kPorterSessionAlphabet) - 2);

    std::string id;
    id.reserve(kPorterSessionIdLength);
    for (std::size_t i = 0; i < kPorterSessionIdLength; ++i) {
        id.push_back(kPorterSessionAlphabet[distribution(generator)]);
    }
    return id;
}

ZeldaNotesLiveMessage zelda_notes_decode_live_message(
    const std::string& payload,
    ZeldaNotesGame game,
    std::chrono::steady_clock::time_point received_at) {
    ZeldaNotesLiveMessage decoded;
    decoded.received_at = received_at;

    try {
        const auto message = Json::parse(payload);
        if (!message.is_object()) return decoded;

        decoded.message_type = message.string("messageType");
        if (decoded.message_type.empty()) return decoded;
        decoded.type = live_message_type(decoded.message_type);
        decoded.valid = true;
        decoded.game_session_id = message.string("gameSessionId");
        decoded.needs_ack = message.boolean("needsAck", false);
        decoded.message_request_id = message.string("messageRequestId");

        if (decoded.type != ZeldaNotesLiveMessageType::MapSyncPlayerInfo) {
            return decoded;
        }

        decoded.updates_live_state = true;
        auto& state = decoded.live_state;
        state.game = game;
        state.received_at = received_at;

        const bool has_position = read_vector3(message, "playerPos", state.position);
        const bool has_front = read_vector3(message, "playerFront", state.front);

        if (game == ZeldaNotesGame::TearsOfTheKingdom) {
            state.layer = zelda_notes_layer_from_wire(message.string("playerLayer"));
            state.synchronized =
                has_position && has_front && state.layer != ZeldaNotesLayer::Unknown;
        } else if (game == ZeldaNotesGame::BreathOfTheWild) {
            state.layer = ZeldaNotesLayer::Ground;
            state.synchronized = has_position && has_front;
        }
        return decoded;
    } catch (...) {
        return ZeldaNotesLiveMessage{};
    }
}

bool zelda_notes_live_state_is_fresh(
    const ZeldaNotesLiveState& state,
    std::chrono::steady_clock::time_point now) {
    if (!state.synchronized ||
        state.received_at == std::chrono::steady_clock::time_point{} ||
        now < state.received_at) {
        return false;
    }
    return now - state.received_at < kZeldaNotesLiveFreshness;
}

void zelda_notes_note_discord_presence(
    const std::string& title_id,
    const std::string& game_name,
    bool playing) {
    log_zelda("note_discord_presence: title_id=" + title_id + " game_name=" + game_name + " playing=" + (playing ? "true" : "false"));
    ZeldaNotesClient* client = nullptr;
    {
        std::lock_guard lock(g_bridge_mutex);
        client = g_client;
    }
    if (client == nullptr) {
        log_zelda("g_client is null");
        return;
    }
    const auto game = playing ? game_from_presence(title_id, game_name) : ZeldaNotesGame::Unknown;
    log_zelda("resolved game=" + std::to_string(static_cast<int>(game)));
    client->set_active_game(game);
}

ZeldaNotesPresence zelda_notes_current_live_presence() {
    ZeldaNotesClient* client = nullptr;
    {
        std::lock_guard lock(g_bridge_mutex);
        client = g_client;
    }
    return client == nullptr ? ZeldaNotesPresence{} : client->live_presence();
}

void zelda_notes_set_rpc_refresh_callback(ZeldaNotesRpcRefreshCallback callback) {
    std::lock_guard lock(g_bridge_mutex);
    g_rpc_refresh_callback = std::move(callback);
}

ZeldaNotesClient::ZeldaNotesClient(HttpClient& http) : http_(http) {
    std::lock_guard lock(g_bridge_mutex);
    g_client = this;
}

ZeldaNotesClient::~ZeldaNotesClient() {
    stop_live_session();
    std::lock_guard lock(g_bridge_mutex);
    if (g_client == this) g_client = nullptr;
}

void ZeldaNotesClient::set_locale(
    const std::string& language,
    const std::string& country) {
    const auto next_language = language.empty() ? std::string("en-GB") : language;
    const auto next_country = country.empty() ? std::string("GB") : country;
    ZeldaNotesGame active_game = ZeldaNotesGame::Unknown;
    {
        std::lock_guard lock(mutex_);
        if (language_ == next_language && country_ == next_country) return;
        language_ = next_language;
        country_ = next_country;
        source_web_token_.clear();
        session_cookie_.clear();
        session_expires_at_ = {};
    }
    {
        std::lock_guard lock(live_mutex_);
        active_game = live_game_;
    }
    if (active_game != ZeldaNotesGame::Unknown) {
        stop_live_session();
        set_active_game(active_game);
    }
}

bool ZeldaNotesClient::ensure_session(const std::string& web_service_token) {
    if (web_service_token.empty()) return false;

    std::string language;
    std::string country;
    {
        std::lock_guard lock(mutex_);
        language = language_;
        country = country_;
        if (source_web_token_ == web_service_token && !session_cookie_.empty() &&
            std::chrono::system_clock::now() < session_expires_at_) {
            return true;
        }
    }

    const auto locale_query = "?lang=" + language + "&na_country=" + country +
        "&na_lang=" + language;
    auto bootstrap = http_.get(
        std::string(kBaseUrl) + "/" + locale_query,
        bootstrap_headers(web_service_token, language, country),
        10,
        8 * 1024 * 1024);
    log_zelda("ensure_session / HTTP " + std::to_string(bootstrap.status));
    auto cookie = session_cookie(bootstrap);
    if (cookie.empty() && (bootstrap.status / 100 == 3)) {
        auto location = header_value(bootstrap, "location");
        if (!location.empty()) {
            if (location.find("://") == std::string::npos) {
                if (location.front() != '/') location = "/" + location;
                location = std::string(kBaseUrl) + location;
            }
            if (location.find('?') == std::string::npos) {
                location += locale_query;
            }
            bootstrap = http_.get(
                location,
                bootstrap_headers(web_service_token, language, country),
                10,
                8 * 1024 * 1024);
            log_zelda("ensure_session redirect to " + location + " HTTP " + std::to_string(bootstrap.status));
            cookie = session_cookie(bootstrap);
        }
    }
    if (cookie.empty()) {
        log_zelda("ensure_session: no session cookie in response");
        for (const auto& [k, v] : bootstrap.headers) {
            log_zelda("header: " + k + " = " + v);
        }
        return false;
    }

    std::lock_guard lock(mutex_);
    if (language_ != language || country_ != country) return false;
    source_web_token_ = web_service_token;
    session_cookie_ = cookie;
    session_expires_at_ = std::chrono::system_clock::now() + kSessionTtl;
    log_zelda("ensure_session success, cookie=" + cookie.substr(0, 15) + "...");
    return true;
}

void ZeldaNotesClient::clear_cache() {
    stop_live_session();
    std::lock_guard lock(mutex_);
    source_web_token_.clear();
    session_cookie_.clear();
    session_expires_at_ = {};
    latest_web_token_.clear();
}

ZeldaNotesPresence ZeldaNotesClient::fetch_presence(
    const std::string& web_service_token) {
    if (web_service_token.empty()) return {};
    log_zelda("fetch_presence called with token");
    {
        std::lock_guard lock(mutex_);
        latest_web_token_ = web_service_token;
    }
    try {
        ensure_session(web_service_token);
    } catch (...) {
    }
    return {};
}

void ZeldaNotesClient::set_active_game(ZeldaNotesGame game) {
    if (game == ZeldaNotesGame::Unknown) {
        stop_live_session();
        return;
    }

    std::string web_token;
    {
        std::lock_guard lock(mutex_);
        web_token = latest_web_token_;
    }
    log_zelda("set_active_game: game=" + std::to_string(static_cast<int>(game)) + " web_token_empty=" + (web_token.empty() ? "true" : "false"));
    if (web_token.empty()) return;

    {
        std::lock_guard lock(live_mutex_);
        if (live_game_ == game && live_thread_.joinable() && !live_stop_.load()) {
            return;
        }
    }

    stop_live_session();
    {
        std::lock_guard lock(live_mutex_);
        live_game_ = game;
        live_stop_.store(false);
        live_thread_ = std::thread(
            [this, game, web_token] { run_live_session(game, web_token); });
    }
}

void ZeldaNotesClient::stop_live_session() {
    std::thread thread;
    {
        std::lock_guard lock(live_mutex_);
        live_stop_.store(true);
        live_cv_.notify_all();
        if (live_thread_.joinable()) thread = std::move(live_thread_);
        live_game_ = ZeldaNotesGame::Unknown;
    }
    if (thread.joinable()) thread.join();
    publish_live_presence({});
}

ZeldaNotesPresence ZeldaNotesClient::live_presence() const {
    std::lock_guard lock(live_mutex_);
    return live_presence_;
}

void ZeldaNotesClient::publish_live_presence(ZeldaNotesPresence presence) {
    log_zelda("publish_live_presence: active=" + std::to_string(presence.active) + " state=" + presence.title_name + " details=" + presence.profile_summary + " img=" + presence.stage_image_uri);
    bool changed = false;
    {
        std::lock_guard lock(live_mutex_);
        changed =
            live_presence_.active != presence.active ||
            live_presence_.title_name != presence.title_name ||
            live_presence_.profile_summary != presence.profile_summary ||
            live_presence_.stage_image_uri != presence.stage_image_uri;
        if (changed) live_presence_ = std::move(presence);
    }
    if (changed) notify_rpc_refresh();
}

void ZeldaNotesClient::run_live_session(
    ZeldaNotesGame game,
    std::string web_service_token) {
    int backoff_seconds = 2;
    WebMetadata metadata;

    log_zelda("run_live_session thread started for game=" + std::to_string(static_cast<int>(game)));
    while (!live_stop_.load()) {
        const auto porter_session_id = zelda_notes_generate_porter_session_id();
        log_zelda("run_live_session loop iteration, porter_session_id=" + porter_session_id + " backoff=" + std::to_string(backoff_seconds));
        bool healthy_stream = false;
        try {
            if (!ensure_session(web_service_token)) {
                log_zelda("ensure_session failed inside run_live_session");
                throw std::runtime_error("Zelda Notes session bootstrap failed");
            }
            std::string session;
            std::string language;
            std::string country;
            {
                std::lock_guard lock(mutex_);
                session = session_cookie_;
                language = language_;
                country = country_;
            }
            if (session.empty()) throw std::runtime_error("Zelda Notes session unavailable");

            if (!metadata.protocol_ready()) {
                log_zelda("run_live_session: discovering web metadata...");
                metadata = discover_web_metadata(
                    http_, game, session, language, country);
            }
            log_zelda("run_live_session: metadata protocol_ready=" + std::string(metadata.protocol_ready() ? "true" : "false"));
            if (!metadata.protocol_ready()) {
                throw std::runtime_error("Zelda Notes map-sync actions unavailable");
            }

            const auto sse_url =
                std::string(kBaseUrl) + "/continuous-connection/sse?gameId=" +
                zelda_notes_game_id(game) + "&porterSessionId=" + porter_session_id;

            log_zelda("run_live_session: connecting SSE to " + sse_url);
            auto sse_headers = authenticated_headers(
                session, language, country, "text/event-stream");
            sse_headers.push_back("Cache-Control: no-cache");
            sse_headers.push_back("Pragma: no-cache");
            sse_headers.push_back("Referer: " + route_url(game));

            SseClient sse(http_.proxy_url());
            std::string game_session_id;
            ZeldaNotesResolvedLocation previous_location;
            auto last_message = std::chrono::steady_clock::now();
            bool protocol_failure = false;

            const auto response = sse.stream(
                sse_url,
                sse_headers,
                [&](const ServerSentEvent& event) {
                    if (live_stop_.load()) return false;
                    log_zelda("SSE event raw data: " + event.data);
                    const auto received_at = std::chrono::steady_clock::now();
                    const auto message = zelda_notes_decode_live_message(
                        event.data, game, received_at);
                    if (!message.valid) return true;
                    last_message = received_at;

                    if (!message.game_session_id.empty()) {
                        game_session_id = message.game_session_id;
                    }
                    if (message.needs_ack && !message.message_request_id.empty()) {
                        try {
                            send_server_action(
                                http_, metadata, game, session, language, country,
                                metadata.ack_action,
                                Json::array{Json(message.message_request_id)});
                        } catch (...) {
                            std::cerr << "[ZeldaNotes] ACK request failed\n";
                        }
                    }

                    if (message.type == ZeldaNotesLiveMessageType::Open) {
                        if (!game_session_id.empty()) {
                            try {
                                send_server_action(
                                    http_, metadata, game, session, language, country,
                                    metadata.end_action,
                                    Json::array{
                                        Json(std::string(zelda_notes_game_id(game))),
                                        Json(porter_session_id),
                                        Json(game_session_id),
                                    });
                            } catch (...) {
                            }
                            game_session_id.clear();
                        }
                        bool started = false;
                        try {
                            started = send_server_action(
                                http_, metadata, game, session, language, country,
                                metadata.start_action,
                                Json::array{
                                    Json(std::string(zelda_notes_game_id(game))),
                                    Json(porter_session_id),
                                    Json("complete-guide"),
                                });
                        } catch (...) {
                        }
                        if (!started) {
                            protocol_failure = true;
                            return false;
                        }
                    } else if (message.updates_live_state) {
                        if (!zelda_notes_live_state_is_fresh(
                                message.live_state, received_at)) {
                            previous_location = {};
                            publish_live_presence({});
                        } else {
                            healthy_stream = true;
                            const auto location = resolve_location(
                                metadata, message.live_state, previous_location);
                            previous_location = location;
                            publish_live_presence(
                                format_rpc_presence(metadata, message.live_state, location));
                        }
                    }
                    return true;
                },
                [&] {
                    if (live_stop_.load()) return true;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_message >= kZeldaNotesLiveFreshness) {
                        publish_live_presence({});
                    }
                    return now - last_message >= std::chrono::minutes(10);
                },
                20,
                1024 * 1024);

            if (!game_session_id.empty()) {
                try {
                    send_server_action(
                        http_, metadata, game, session, language, country,
                        metadata.end_action,
                        Json::array{
                            Json(std::string(zelda_notes_game_id(game))),
                            Json(porter_session_id),
                            Json(game_session_id),
                        });
                } catch (...) {
                }
            }

            publish_live_presence({});
            if (live_stop_.load()) break;
            if (response.status != 0 && response.status / 100 != 2) {
                protocol_failure = true;
            }
            if (protocol_failure) metadata = {};
            backoff_seconds = 2;
        } catch (const std::exception& error) {
            if (!live_stop_.load()) {
                log_zelda("Live map sync unavailable: " + std::string(error.what()));
            }
            publish_live_presence({});
            backoff_seconds = 2;
        } catch (...) {
            log_zelda("Live map sync unknown error");
            publish_live_presence({});
            backoff_seconds = 2;
        }

        if (live_stop_.load()) break;
        std::unique_lock wait_lock(live_mutex_);
        live_cv_.wait_for(
            wait_lock,
            std::chrono::seconds(backoff_seconds),
            [this] { return live_stop_.load(); });
        wait_lock.unlock();
    }
}

}  // namespace nso
