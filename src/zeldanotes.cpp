#include "nso_album_sync/zeldanotes.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/sse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    std::map<std::string, std::string> labels;
    std::vector<MapPlace> places;

    bool protocol_ready() const {
        return !start_action.empty() && !end_action.empty() && !ack_action.empty();
    }
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

    const auto page = http.get(
        page_url,
        authenticated_headers(
            session, language, country,
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
        12,
        kMaxRouteHtmlBytes);
    if (page.status / 100 != 2) return metadata;

    const auto html = page.text();
    metadata.start_action = find_server_reference(html, "sendMapSyncStartAction");
    metadata.end_action = find_server_reference(html, "sendMapSyncEndAction");
    metadata.ack_action = find_server_reference(html, "sendAckAction");
    metadata.deployment_id = header_value(page, "x-deployment-id");

    const auto scripts = extract_script_urls(html);
    for (const auto& script_url : scripts) {
        if (metadata.deployment_id.empty()) {
            metadata.deployment_id = deployment_id_from_url(script_url);
        }
        try {
            const auto script_response = http.get(
                script_url,
                authenticated_headers(
                    session, language, country,
                    "application/javascript,text/javascript,*/*;q=0.8"),
                12,
                kMaxScriptBytes);
            if (script_response.status / 100 != 2) continue;
            const auto script = script_response.text();
            if (metadata.start_action.empty()) {
                metadata.start_action =
                    find_server_reference(script, "sendMapSyncStartAction");
            }
            if (metadata.end_action.empty()) {
                metadata.end_action =
                    find_server_reference(script, "sendMapSyncEndAction");
            }
            if (metadata.ack_action.empty()) {
                metadata.ack_action = find_server_reference(script, "sendAckAction");
            }
            if (metadata.places.empty()) {
                extract_map_dataset(script, game, metadata.places);
            }
        } catch (...) {
        }
        if (metadata.protocol_ready() && !metadata.places.empty()) break;
    }

    metadata.labels = fetch_labels(http, session, language, country);
    return metadata;
}

bool action_succeeded(const HttpResponse& response) {
    if (response.status / 100 != 2) return false;
    const auto text = response.text();
    return text.find("\"isSuccess\":true") != std::string::npos;
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

    const auto response = http.post(
        page_url,
        Json(arguments).dump(),
        headers,
        "text/plain;charset=UTF-8",
        12);
    return action_succeeded(response);
}

std::pair<std::string, std::string> region_mapping(
    ZeldaNotesGame game,
    const std::string& tower_label) {
    if (game == ZeldaNotesGame::TearsOfTheKingdom) {
        if (tower_label == "Ex_Tower03" || tower_label == "Ex_Tower15") {
            return {"Ex_MapRegion_Hebura", "Hebra"};
        }
        if (tower_label == "Ex_Tower04") return {"Ex_MapRegion_Eldin", "Eldin"};
        if (tower_label == "Ex_Tower05") return {"Ex_MapRegion_Tamul", "Akkala"};
        if (tower_label == "Ex_Tower06" || tower_label == "Ex_Tower11") {
            return {"Ex_MapRegion_Hateru", "Necluda"};
        }
        if (tower_label == "Ex_Tower07" || tower_label == "Ex_Tower14") {
            return {"Ex_MapRegion_Lanayru", "Lanayru"};
        }
        if (tower_label == "Ex_Tower09" || tower_label == "Ex_Tower10") {
            return {"Ex_MapRegion_Gerudo", "Gerudo"};
        }
        if (tower_label == "Ex_Tower13") return {"Ex_MapRegion_Firone", "Faron"};
        if (tower_label == "Ex_Tower01" || tower_label == "Ex_Tower02" ||
            tower_label == "Ex_Tower08" || tower_label == "Ex_Tower12") {
            return {"Ex_MapRegion_HyrulePrairie", "Central Hyrule"};
        }
    } else {
        if (tower_label == "U_Tower01" || tower_label == "U_Tower02") {
            return {"U_MapRegion_Hebura", "Hebra"};
        }
        if (tower_label == "U_Tower03" || tower_label == "U_Tower04") {
            return {"U_MapRegion_Gerudo", "Gerudo"};
        }
        if (tower_label == "U_Tower10") return {"U_MapRegion_Eldin", "Eldin"};
        if (tower_label == "U_Tower11") return {"U_MapRegion_Tamul", "Akkala"};
        if (tower_label == "U_Tower12") return {"U_MapRegion_Lanayru", "Lanayru"};
        if (tower_label == "U_Tower08" || tower_label == "U_Tower13") {
            return {"U_MapRegion_Hateru", "Necluda"};
        }
        if (tower_label == "U_Tower09" || tower_label == "U_Tower14") {
            return {"U_MapRegion_Firone", "Faron"};
        }
        if (tower_label == "U_Tower05" || tower_label == "U_Tower06" ||
            tower_label == "U_Tower07" || tower_label == "U_Tower15") {
            return {"U_MapRegion_HyrulePrairie ", "Central Hyrule"};
        }
    }
    return {};
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
    const MapPlace* nearest = nullptr;
    double nearest_distance = (std::numeric_limits<double>::max)();
    const auto tower_subcategory =
        game == ZeldaNotesGame::TearsOfTheKingdom ? "skyviewTower" : "tower";
    for (const auto& place : metadata.places) {
        if (place.subcategory != tower_subcategory) continue;
        const auto distance = horizontal_distance(position, place.position);
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = &place;
        }
    }
    if (nearest == nullptr) return {};
    const auto mapping = region_mapping(game, nearest->message_label);
    if (mapping.first.empty()) return {};
    const auto localized = localized_label(metadata, mapping.first);
    return localized.empty() ? mapping.second : localized;
}

struct DistanceThresholds {
    double at = 80.0;
    double near = 300.0;
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
        if (distance > thresholds.near) continue;
        const auto priority_penalty =
            static_cast<double>(100 - category_priority(place.subcategory)) * 0.004;
        const auto score = distance / thresholds.near + priority_penalty;
        if (score < best.score) best = Candidate{&place, distance, score};
    }

    // Keep the current POI around boundaries unless a new candidate is
    // substantially closer. This prevents Discord from bouncing A/B/A/B while
    // Link walks between two nearby map markers.
    if (previous.poi_uid != 0 && best.place != nullptr &&
        best.place->uid != previous.poi_uid) {
        for (const auto& place : metadata.places) {
            if (place.uid != previous.poi_uid || !same_layer(place, state)) continue;
            const auto thresholds = thresholds_for(place.subcategory);
            const auto distance = horizontal_distance(state.position, place.position);
            if (distance <= thresholds.near * 1.15 &&
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

ZeldaNotesPresence format_rpc_presence(
    const ZeldaNotesLiveState& state,
    const ZeldaNotesResolvedLocation& location) {
    ZeldaNotesPresence presence;
    if (!location.valid) return presence;

    std::string details;
    std::string secondary;
    if (!location.poi.empty() && location.at_poi) {
        details = "At " + location.poi;
        if (state.game == ZeldaNotesGame::TearsOfTheKingdom) {
            if (state.layer == ZeldaNotesLayer::Underground) {
                secondary = "The Depths";
                if (!location.region.empty()) secondary += " • " + location.region;
            } else if (!location.region.empty()) {
                secondary = location.region + " • " + zelda_notes_layer_rpc_name(state.layer);
            } else {
                secondary = zelda_notes_layer_rpc_name(state.layer);
            }
        } else {
            secondary = location.region.empty() ? "Hyrule" : location.region;
        }
    } else if (!location.poi.empty()) {
        if (state.game == ZeldaNotesGame::TearsOfTheKingdom &&
            state.layer == ZeldaNotesLayer::Sky) {
            details = "Exploring the Sky";
        } else if (state.game == ZeldaNotesGame::TearsOfTheKingdom &&
                   state.layer == ZeldaNotesLayer::Underground) {
            details = "Exploring the Depths";
        } else if (!location.region.empty()) {
            details = "Exploring " + location.region;
        } else if (state.game == ZeldaNotesGame::TearsOfTheKingdom) {
            details = "Exploring the Surface";
        } else {
            details = "Exploring Hyrule";
        }
        secondary = "Near " + location.poi;
    } else if (state.game == ZeldaNotesGame::TearsOfTheKingdom) {
        if (state.layer == ZeldaNotesLayer::Sky && !location.region.empty()) {
            details = "Exploring the Sky";
            secondary = "Above " + location.region;
        } else if (state.layer == ZeldaNotesLayer::Underground && !location.region.empty()) {
            details = "Exploring the Depths";
            secondary = "Below " + location.region;
        } else if (state.layer == ZeldaNotesLayer::Ground && !location.region.empty()) {
            details = "Exploring " + location.region;
            secondary = "Surface";
        }
    } else if (!location.region.empty()) {
        details = "Exploring " + location.region;
        secondary = "Hyrule";
    }

    if (details.empty() || secondary.empty()) return {};
    presence.profile_summary = clamp_activity_text(details);
    presence.title_name = clamp_activity_text(secondary);
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
    ZeldaNotesClient* client = nullptr;
    {
        std::lock_guard lock(g_bridge_mutex);
        client = g_client;
    }
    if (client == nullptr) return;
    client->set_active_game(
        playing ? game_from_presence(title_id, game_name) : ZeldaNotesGame::Unknown);
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
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (language_ == next_language && country_ == next_country) return;
        language_ = next_language;
        country_ = next_country;
        source_web_token_.clear();
        session_cookie_.clear();
        session_expires_at_ = {};
        changed = true;
    }
    if (changed) {
        ZeldaNotesGame game;
        {
            std::lock_guard lock(live_mutex_);
            game = live_game_;
        }
        if (game != ZeldaNotesGame::Unknown) set_active_game(game);
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
    const auto bootstrap = http_.get(
        std::string(kBaseUrl) + "/title-select" + locale_query,
        bootstrap_headers(web_service_token, language, country),
        10,
        8 * 1024 * 1024);
    if (bootstrap.status / 100 != 2 && bootstrap.status / 100 != 3) return false;
    const auto cookie = session_cookie(bootstrap);
    if (cookie.empty()) return false;

    std::lock_guard lock(mutex_);
    if (language_ != language || country_ != country) return false;
    source_web_token_ = web_service_token;
    session_cookie_ = cookie;
    session_expires_at_ = std::chrono::system_clock::now() + kSessionTtl;
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
    {
        std::lock_guard lock(mutex_);
        latest_web_token_ = web_service_token;
    }
    try {
        ensure_session(web_service_token);
    } catch (...) {
    }
    // App's existing game-service path remains one-shot. The Discord renderer
    // immediately tells this client which Zelda title Coral reported, at which
    // point the independent live stream begins and pushes meaningful changes.
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
    bool changed = false;
    {
        std::lock_guard lock(live_mutex_);
        changed =
            live_presence_.active != presence.active ||
            live_presence_.title_name != presence.title_name ||
            live_presence_.profile_summary != presence.profile_summary;
        if (changed) live_presence_ = std::move(presence);
    }
    if (changed) notify_rpc_refresh();
}

void ZeldaNotesClient::run_live_session(
    ZeldaNotesGame game,
    std::string web_service_token) {
    const auto porter_session_id = zelda_notes_generate_porter_session_id();
    int backoff_seconds = 2;
    WebMetadata metadata;

    while (!live_stop_.load()) {
        try {
            if (!ensure_session(web_service_token)) {
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

            if (!metadata.protocol_ready() || metadata.places.empty() || metadata.labels.empty()) {
                metadata = discover_web_metadata(
                    http_, game, session, language, country);
            }
            if (!metadata.protocol_ready()) {
                throw std::runtime_error("Zelda Notes map-sync actions unavailable");
            }
            if (metadata.places.empty() || metadata.labels.empty()) {
                throw std::runtime_error("Zelda Notes Complete Guide data unavailable");
            }

            const auto sse_url = std::string(kBaseUrl) +
                "/continuous-connection/sse?gameId=" + zelda_notes_game_id(game) +
                "&porterSessionId=" + porter_session_id;
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
                            const auto location = resolve_location(
                                metadata, message.live_state, previous_location);
                            previous_location = location;
                            publish_live_presence(
                                format_rpc_presence(message.live_state, location));
                        }
                    }
                    return true;
                },
                [&] {
                    if (live_stop_.load()) return true;
                    return std::chrono::steady_clock::now() - last_message >=
                        kZeldaNotesLiveFreshness;
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
                std::cerr << "[ZeldaNotes] Live map sync unavailable: "
                          << error.what() << '\n';
            }
            publish_live_presence({});
        } catch (...) {
            publish_live_presence({});
        }

        if (live_stop_.load()) break;
        std::unique_lock wait_lock(live_mutex_);
        live_cv_.wait_for(
            wait_lock,
            std::chrono::seconds(backoff_seconds),
            [this] { return live_stop_.load(); });
        wait_lock.unlock();
        backoff_seconds = next_backoff(backoff_seconds);
    }
}

}  // namespace nso
