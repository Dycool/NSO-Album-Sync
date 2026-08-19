#pragma once

#include "nso_album_sync/http.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kZeldaNotesGameServiceId = 5935781783175168ULL;
inline constexpr std::uint64_t kZeldaNotesGameServiceIdAlt = 4974384874151936ULL;

inline constexpr char kZeldaNotesBotwTitleId[] = "01007ef00011e000";
inline constexpr char kZeldaNotesTotkTitleId[] = "0100f2c0115b6000";
inline constexpr auto kZeldaNotesLiveFreshness = std::chrono::seconds(30);

enum class ZeldaNotesGame {
    Unknown,
    BreathOfTheWild,
    TearsOfTheKingdom,
};

enum class ZeldaNotesLayer {
    Unknown,
    Ground,
    Sky,
    Underground,
};

enum class ZeldaNotesLiveMessageType {
    Unknown,
    Open,
    MapSyncStartAck,
    MapSyncPlayerInfo,
};

struct ZeldaNotesVector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Live Complete Guide / Navigation state. This is deliberately separate from
// ZeldaNotesPresence: unlike one-shot game-service enrichment, map sync is a
// continuously refreshed data source and becomes unusable as soon as it is no
// longer synchronized/fresh.
struct ZeldaNotesLiveState {
    ZeldaNotesGame game = ZeldaNotesGame::Unknown;
    ZeldaNotesLayer layer = ZeldaNotesLayer::Unknown;
    ZeldaNotesVector3 position;
    ZeldaNotesVector3 front;
    bool synchronized = false;
    std::chrono::steady_clock::time_point received_at{};
};

// One decoded message from Zelda Notes' continuous-connection SSE stream.
// `received_at` belongs to the wire message itself so the future live-session
// coordinator can reproduce Nintendo's 30-second "any message" watchdog. A
// map_sync_player_info message additionally sets updates_live_state and carries
// the current synchronized/unsynchronized player state.
struct ZeldaNotesLiveMessage {
    ZeldaNotesLiveMessageType type = ZeldaNotesLiveMessageType::Unknown;
    std::string game_session_id;
    bool needs_ack = false;
    std::string message_request_id;
    bool updates_live_state = false;
    ZeldaNotesLiveState live_state;
    bool valid = false;
    std::chrono::steady_clock::time_point received_at{};
};

// Human-readable location derived locally from the verified Zelda Notes map
// data. Raw Nintendo coordinates should never be exposed directly to Discord.
struct ZeldaNotesResolvedLocation {
    ZeldaNotesLayer layer = ZeldaNotesLayer::Unknown;
    std::string region;
    std::string poi;
    bool valid = false;
    bool at_poi = false;
    bool near_poi = false;
};

inline ZeldaNotesGame zelda_notes_game_for_title_id(const std::string& title_id) {
    if (title_id == kZeldaNotesBotwTitleId) {
        return ZeldaNotesGame::BreathOfTheWild;
    }
    if (title_id == kZeldaNotesTotkTitleId) {
        return ZeldaNotesGame::TearsOfTheKingdom;
    }
    return ZeldaNotesGame::Unknown;
}

inline const char* zelda_notes_game_id(ZeldaNotesGame game) {
    switch (game) {
        case ZeldaNotesGame::BreathOfTheWild:
            return "0";
        case ZeldaNotesGame::TearsOfTheKingdom:
            return "1";
        case ZeldaNotesGame::Unknown:
            return "";
    }
    return "";
}

inline const char* zelda_notes_short_name(ZeldaNotesGame game) {
    switch (game) {
        case ZeldaNotesGame::BreathOfTheWild:
            return "botw";
        case ZeldaNotesGame::TearsOfTheKingdom:
            return "totk";
        case ZeldaNotesGame::Unknown:
            return "";
    }
    return "";
}

inline const char* zelda_notes_layer_rpc_name(ZeldaNotesLayer layer) {
    switch (layer) {
        case ZeldaNotesLayer::Ground:
            return "Surface";
        case ZeldaNotesLayer::Sky:
            return "Sky";
        case ZeldaNotesLayer::Underground:
            return "Depths";
        case ZeldaNotesLayer::Unknown:
            return "";
    }
    return "";
}

ZeldaNotesLayer zelda_notes_layer_from_wire(const std::string& layer);
std::string zelda_notes_generate_porter_session_id();

ZeldaNotesLiveMessage zelda_notes_decode_live_message(
    const std::string& payload,
    ZeldaNotesGame game,
    std::chrono::steady_clock::time_point received_at =
        std::chrono::steady_clock::now());

bool zelda_notes_live_state_is_fresh(
    const ZeldaNotesLiveState& state,
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now());

struct ZeldaNotesPresence {
    // Legacy synchronous enrichment result. The live location implementation
    // uses ZeldaNotesLiveState instead and must not freeze a location here for
    // the rest of the play session.
    std::string title_name;
    std::string profile_summary;
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const { return title_name; }
    std::string format_details() const { return profile_summary; }
};

class ZeldaNotesClient {
public:
    explicit ZeldaNotesClient(HttpClient& http) : http_(http) {}

    ZeldaNotesPresence fetch_presence(const std::string& web_service_token);

    void set_locale(const std::string& language, const std::string& country) {
        const auto next_language = language.empty() ? std::string("en-GB") : language;
        const auto next_country = country.empty() ? std::string("GB") : country;
        std::lock_guard lock(mutex_);
        if (language_ == next_language && country_ == next_country) return;
        language_ = next_language;
        country_ = next_country;
        source_web_token_.clear();
        session_cookie_.clear();
        session_expires_at_ = {};
    }

    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string language_ = "en-GB";
    std::string country_ = "GB";
    std::string source_web_token_;
    std::string session_cookie_;
    std::chrono::system_clock::time_point session_expires_at_{};
};

}  // namespace nso
