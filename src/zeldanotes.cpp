#include "nso_album_sync/zeldanotes.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/sse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <string>
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
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string session_cookie(const HttpResponse& response) {
    for (const auto& line : set_cookie_lines(response)) {
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        const auto eq = line.find('=', start);
        if (eq == std::string::npos) continue;
        const auto name = line.substr(start, eq - start);
        auto lower_name = name;
        std::transform(
            lower_name.begin(), lower_name.end(), lower_name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_name != "a5_token" && lower_name.find("session") == std::string::npos) {
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

        decoded.type = live_message_type(message.string("messageType"));
        if (decoded.type == ZeldaNotesLiveMessageType::Unknown) return decoded;
        decoded.valid = true;

        // The captured Nintendo frontend forwards these identifiers unchanged
        // into its server actions. In the verified traffic they are strings, so
        // do not silently coerce other JSON types into action arguments.
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
            // Zelda Notes' TOTK PlayerProvider only treats the stream as live
            // when all three fields exist: playerPos, playerFront, playerLayer.
            state.synchronized =
                has_position && has_front && state.layer != ZeldaNotesLayer::Unknown;
        } else if (game == ZeldaNotesGame::BreathOfTheWild) {
            // BOTW has no layer field; its PlayerProvider requires only the
            // current player position and facing vector.
            state.synchronized = has_position && has_front;
        }

        return decoded;
    } catch (...) {
        // A malformed individual SSE payload must never take down the presence
        // worker. The live-session coordinator can ignore invalid messages and
        // rely on the freshness watchdog to fall back safely.
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

void ZeldaNotesClient::clear_cache() {
    std::lock_guard lock(mutex_);
    source_web_token_.clear();
    session_cookie_.clear();
    session_expires_at_ = {};
}

ZeldaNotesPresence ZeldaNotesClient::fetch_presence(const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    try {
        std::string language;
        std::string country;
        {
            std::lock_guard lock(mutex_);
            language = language_;
            country = country_;
            if (source_web_token_ == web_service_token && !session_cookie_.empty() &&
                std::chrono::system_clock::now() < session_expires_at_) {
                // The live Complete Guide stream is managed separately. Do not
                // keep reloading a page from this legacy synchronous probe just
                // to rediscover the same authenticated Zelda Notes cookie.
                return {};
            }
        }

        const auto locale_query = "?lang=" + language + "&na_country=" + country +
            "&na_lang=" + language;

        // The working backend resolves its proxy root to /title-select before
        // making any Nintendo request. Therefore /title-select is the actual
        // first Nintendo navigation that receives GameWebServiceToken and
        // establishes Zelda Notes' a5_token/session cookie.
        const auto bootstrap = http_.get(
            std::string(kBaseUrl) + "/title-select" + locale_query,
            bootstrap_headers(web_service_token, language, country),
            10,
            8 * 1024 * 1024);
        if (bootstrap.status / 100 != 2 && bootstrap.status / 100 != 3) return {};
        const auto cookie = session_cookie(bootstrap);
        if (cookie.empty()) return {};

        std::lock_guard lock(mutex_);
        if (language_ != language || country_ != country) return {};
        source_web_token_ = web_service_token;
        session_cookie_ = cookie;
        session_expires_at_ = std::chrono::system_clock::now() + kSessionTtl;

        // Authentication alone is not current-location evidence. A later step
        // starts the verified continuous-connection stream and only publishes
        // synchronized, fresh map_sync_player_info data.
        return {};
    } catch (...) {
        return {};
    }
}

}  // namespace nso
