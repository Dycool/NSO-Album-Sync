#pragma once

#include "nso_album_sync/zeldanotes.hpp"

#include <string>

namespace nso {

struct ZeldaLocationResult {
    std::string name;
    std::string image_url;
    std::string category;
    ZeldaNotesLayer layer = ZeldaNotesLayer::Unknown;
    bool matched = false;
};

// Evaluates Link's 3D position against TotK 3D collision volumes (villages, stables,
// skyview towers, sky archipelagos, depths abandoned mines).
ZeldaLocationResult resolve_totk_location_3d(
    const ZeldaNotesVector3& pos,
    ZeldaNotesLayer layer = ZeldaNotesLayer::Unknown);

// Evaluates Link's 3D position against BotW 3D collision volumes (villages, stables, towers).
ZeldaLocationResult resolve_botw_location_3d(const ZeldaNotesVector3& pos);

// Returns specific location artwork for any recognized POI/landmark name.
std::string resolve_poi_artwork(
    const std::string& poi_name,
    ZeldaNotesGame game);

// Returns high-resolution Zelda Wiki region artwork for broad area exploration.
std::string resolve_zelda_region_artwork(
    const std::string& region,
    ZeldaNotesGame game,
    ZeldaNotesLayer layer = ZeldaNotesLayer::Ground);

}  // namespace nso
