#pragma once
#include "nso_album_sync/coral.hpp"
#include "nso_album_sync/rpc_settings.hpp"
#include <chrono>
#include <initializer_list>

namespace nso {
enum class RpcGameService {
    None,
    Splatoon3,
    ZeldaNotes,
    AnimalCrossing,
    Splatoon2,
};

inline bool rpc_contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const auto* needle : needles) {
        if (needle != nullptr && text.find(needle) != std::string::npos) return true;
    }
    return false;
}

inline RpcGameService rpc_game_service_for(const NintendoPresence& presence) {
    // Prefer Nintendo's stable application/title ID. Coral names are localized,
    // so routing only by English/Japanese substrings silently disables RPC
    // enrichment for other Nintendo Account languages.
    if (presence.title_id == "0100c2500fc20000") return RpcGameService::Splatoon3;
    if (presence.title_id == "01003bc0000a0000") return RpcGameService::Splatoon2;
    if (presence.title_id == "01006f8002326000") return RpcGameService::AnimalCrossing;
    if (presence.title_id == "01007ef00011e000" ||
        presence.title_id == "0100f2c0115b6000") {
        return RpcGameService::ZeldaNotes;
    }

    // Name fallbacks cover Coral payloads that omit an application ID and
    // Nintendo Switch 2 Edition display names. Keep Zelda deliberately narrow:
    // Zelda Notes supports BOTW/TOTK, not every Zelda-family title.
    if (rpc_contains_any(presence.game_name, {"Splatoon 3", "スプラトゥーン3"})) {
        return RpcGameService::Splatoon3;
    }
    if (rpc_contains_any(presence.game_name, {
            "Breath of the Wild", "ブレス オブ ザ ワイルド",
            "Tears of the Kingdom", "ティアーズ オブ ザ キングダム"})) {
        return RpcGameService::ZeldaNotes;
    }
    if (rpc_contains_any(presence.game_name, {
            "Animal Crossing", "New Horizons", "どうぶつの森", "あつ森"})) {
        return RpcGameService::AnimalCrossing;
    }
    if (rpc_contains_any(presence.game_name, {"Splatoon 2", "スプラトゥーン2"})) {
        return RpcGameService::Splatoon2;
    }
    return RpcGameService::None;
}

inline bool rpc_service_enabled(RpcGameService service, const RpcSettings& settings) {
    switch (service) {
        case RpcGameService::ZeldaNotes: return settings.zelda;
        case RpcGameService::AnimalCrossing: return settings.animal_crossing;
        case RpcGameService::Splatoon3: return settings.splatoon3;
        case RpcGameService::Splatoon2: return settings.splatoon2;
        case RpcGameService::None: return false;
    }
    return false;
}

// Failed probes retry at a bounded cadence instead of poisoning a play session.
struct RpcEnrichmentCache {
    using Clock = std::chrono::steady_clock;
    std::string game_key;
    std::uint64_t account = 0;
    std::uint64_t revision = 0;
    bool success = false;
    Clock::time_point retry_at{};
    std::string details, details_without_name, state, image, profile_image;

    bool begin(const std::string& key, std::uint64_t generation, std::uint64_t settings_revision) {
        if (game_key == key && account == generation && revision == settings_revision) return false;
        *this = {};
        game_key = key;
        account = generation;
        revision = settings_revision;
        return true;
    }
    bool should_probe(Clock::time_point now) const { return !success && now >= retry_at; }
    void complete(const NintendoPresence& presence, bool ready, Clock::time_point now) {
        success = ready || !presence.custom_details.empty() ||
            !presence.custom_state.empty() || !presence.custom_image_uri.empty();
        details = presence.custom_details;
        details_without_name = presence.custom_details_without_name;
        profile_image = presence.profile_image_uri;
        state = presence.custom_state;
        image = presence.custom_image_uri;
        retry_at = now + std::chrono::minutes(5);
    }
    void apply(NintendoPresence& presence) const {
        presence.custom_details = details;
        presence.custom_details_without_name = details_without_name;
        if (!profile_image.empty()) presence.profile_image_uri = profile_image;
        presence.custom_state = state;
        presence.custom_image_uri = image;
    }
};
inline NintendoPresence rpc_display_presence(NintendoPresence presence) {
    if (!rpc_service_enabled(rpc_game_service_for(presence), presence.rpc)) {
        presence.custom_details.clear();
        presence.custom_details_without_name.clear();
        presence.custom_state.clear();
        presence.custom_image_uri.clear();
        presence.custom_large_image_uri.clear();
        presence.custom_large_text.clear();
    }
    if (!presence.rpc.show_username) {
        presence.user_name.clear();
        presence.custom_details = presence.custom_details_without_name;
    }
    if (!presence.rpc.show_profile_picture) presence.profile_image_uri.clear();
    if (!presence.rpc.show_play_time) {
        presence.sys_description.clear();
        presence.total_play_time = 0;
    }
    if (!presence.rpc.show_elapsed_time) presence.updated_at = 0;
    return presence;
}
}  // namespace nso
