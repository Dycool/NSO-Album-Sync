#pragma once

#include "nso_album_sync/http.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
// Unknown message types can still be valid because Nintendo may request an ACK
// or attach a gameSessionId to messages unrelated to Rich Presence.
struct ZeldaNotesLiveMessage {
    ZeldaNotesLiveMessageType type = ZeldaNotesLiveMessageType::Unknown;
    std::string message_type;
    std::string game_session_id;
    bool needs_ack = false;
    std::string message_request_id;
    bool updates_live_state = false;
    ZeldaNotesLiveState live_state;
    bool valid = false;
    std::chrono::steady_clock::time_point received_at{};
};

// Human-readable location derived locally from Nintendo's Zelda Notes Complete
// Guide data. Raw Nintendo coordinates are never exposed to Discord.
struct ZeldaNotesResolvedLocation {
    ZeldaNotesLayer layer = ZeldaNotesLayer::Unknown;
    std::string region;
    std::string poi;
    std::int64_t poi_uid = 0;
    double poi_distance = 0.0;
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
    // Keep the legacy shape because App's game-service branch already consumes
    // this interface. Live location updates are overlaid again by Discord so the
    // generic game name, artwork, profile image and Coral timer remain intact.
    std::string title_name;
    std::string profile_summary;
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const { return title_name; }
    std::string format_details() const { return profile_summary; }
};

using ZeldaNotesRpcRefreshCallback = std::function<void()>;

// Discord supplies the latest Coral title immediately after the one-shot Zelda
// bootstrap. This lets the Zelda client choose BOTW/TOTK without duplicating or
// accelerating Coral requests. A non-Zelda/offline presence stops map sync.
void zelda_notes_note_discord_presence(
    const std::string& title_id,
    const std::string& game_name,
    bool playing);

// Discord queries this just before rendering. It returns active=true only for a
// synchronized, fresh location that could be resolved to trustworthy text.
ZeldaNotesPresence zelda_notes_current_live_presence();

// The live Zelda worker asks Discord to re-render only when the meaningful
// details/state strings change (or when live location becomes unavailable).
void zelda_notes_set_rpc_refresh_callback(ZeldaNotesRpcRefreshCallback callback);

class ZeldaNotesClient {
public:
    explicit ZeldaNotesClient(HttpClient& http);
    ~ZeldaNotesClient();

    ZeldaNotesClient(const ZeldaNotesClient&) = delete;
    ZeldaNotesClient& operator=(const ZeldaNotesClient&) = delete;

    ZeldaNotesPresence fetch_presence(const std::string& web_service_token);

    void set_locale(const std::string& language, const std::string& country);
    void clear_cache();

    // Internal bridge used by zelda_notes_note_discord_presence(). Public only
    // so the process-wide single-client bridge does not need friend plumbing.
    void set_active_game(ZeldaNotesGame game);
    ZeldaNotesPresence live_presence() const;

private:
    bool ensure_session(const std::string& web_service_token);
    void stop_live_session();
    void run_live_session(ZeldaNotesGame game, std::string web_service_token);
    void publish_live_presence(ZeldaNotesPresence presence);

    HttpClient& http_;

    mutable std::mutex mutex_;
    std::string language_ = "en-GB";
    std::string country_ = "GB";
    std::string source_web_token_;
    std::string session_cookie_;
    std::chrono::system_clock::time_point session_expires_at_{};
    std::string latest_web_token_;

    mutable std::mutex live_mutex_;
    std::condition_variable live_cv_;
    std::thread live_thread_;
    std::atomic<bool> live_stop_{false};
    ZeldaNotesGame live_game_ = ZeldaNotesGame::Unknown;
    ZeldaNotesPresence live_presence_;
};

}  // namespace nso
