#include "nso_album_sync/zeldanotes_regions.hpp"

#include <cmath>
#include <vector>

namespace nso {
namespace {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

enum class ShapeType {
    Sphere,
    Box,
    Cylinder,
    Capsule,
};

struct ShapeArea {
    ShapeType type = ShapeType::Sphere;
    Vec3 center;
    double radius = 0.0;
    Vec3 half_extents;
    double rotation_y = 0.0;
    double half_height = 0.0;
};

ShapeArea make_sphere(Vec3 center, double radius) {
    ShapeArea a;
    a.type = ShapeType::Sphere;
    a.center = center;
    a.radius = radius;
    return a;
}

ShapeArea make_box(Vec3 center, Vec3 half_extents, double rotation_y) {
    ShapeArea a;
    a.type = ShapeType::Box;
    a.center = center;
    a.half_extents = half_extents;
    a.rotation_y = rotation_y;
    return a;
}

ShapeArea make_cylinder(Vec3 center, double radius, double half_height) {
    ShapeArea a;
    a.type = ShapeType::Cylinder;
    a.center = center;
    a.radius = radius;
    a.half_height = half_height;
    return a;
}

ShapeArea make_capsule(Vec3 center, double radius, double half_height) {
    ShapeArea a;
    a.type = ShapeType::Capsule;
    a.center = center;
    a.radius = radius;
    a.half_height = half_height;
    return a;
}

bool is_within_shape(const ZeldaNotesVector3& pos, const ShapeArea& area) {
    const double dx = pos.x - area.center.x;
    const double dy = pos.y - area.center.y;
    const double dz = pos.z - area.center.z;

    if (area.type == ShapeType::Sphere) {
        return (dx * dx + dy * dy + dz * dz) <= (area.radius * area.radius);
    }
    if (area.type == ShapeType::Box) {
        const double c = std::cos(area.rotation_y);
        const double s = std::sin(area.rotation_y);
        const double local_x = c * dx - s * dz;
        const double local_z = s * dx + c * dz;
        return std::abs(local_x) <= area.half_extents.x &&
               std::abs(dy) <= area.half_extents.y &&
               std::abs(local_z) <= area.half_extents.z;
    }
    if (area.type == ShapeType::Cylinder) {
        return (dx * dx + dz * dz) <= (area.radius * area.radius) &&
               std::abs(dy) <= area.half_height;
    }
    if (area.type == ShapeType::Capsule) {
        const double nearest_y = std::max(-area.half_height, std::min(dy, area.half_height));
        const double dist_y = dy - nearest_y;
        return (dx * dx + dist_y * dist_y + dz * dz) <= (area.radius * area.radius);
    }
    return false;
}

struct LocationDefinition {
    const char* name;
    const char* image_url;
    const char* category;
    ZeldaNotesLayer layer = ZeldaNotesLayer::Ground;
    Vec3 center;
    double bounding_radius = 0.0;
    std::vector<ShapeArea> areas;
};

bool is_within_location(
    const ZeldaNotesVector3& pos,
    const LocationDefinition& loc,
    ZeldaNotesLayer player_layer) {
    if (player_layer != ZeldaNotesLayer::Unknown && loc.layer != ZeldaNotesLayer::Unknown) {
        if (player_layer != loc.layer) return false;
    }
    const double dx = pos.x - loc.center.x;
    const double dz = pos.z - loc.center.z;
    if (loc.bounding_radius > 0.0 && (dx * dx + dz * dz) > (loc.bounding_radius * loc.bounding_radius)) {
        return false;
    }
    if (loc.areas.empty()) {
        return true;
    }
    for (const auto& area : loc.areas) {
        if (is_within_shape(pos, area)) return true;
    }
    return false;
}

constexpr double kTotkSkyMinY = 500.0;
constexpr double kTotkDepthsMaxY = -300.0;
constexpr double kAbandonedMineRadius = 200.0;
constexpr double kAbandonedMineHalfHeight = 180.0;
constexpr double kBotwTowerRadius = 25.0;
constexpr double kBotwTowerHalfHeight = 34.537;

// ============================================================================
// Tears of the Kingdom Location Definitions
// ============================================================================

const std::vector<LocationDefinition>& get_totk_locations() {
    static const std::vector<LocationDefinition> locations = {
        // Settlements & Villages
        {
            "Lookout Landing",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/94/TotK_Lookout_Landing.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-254.12, 123.447, -101.602},
            83.45,
            {make_box({-254.12, 123.447, -101.602}, {55.012, 16.807, 62.732}, 0.0)}
        },
        {
            "Tarrey Town",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3964.299, 152.472, -1612.209},
            85.0,
            {make_sphere({3964.299, 152.472, -1612.209}, 85.0)}
        },
        {
            "Rito Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3501, 215, -1835},
            210.0,
            {
                make_sphere({-3620.765, 208.672, -1799.816}, 80.0),
                make_sphere({-3482, 208.672, -1841.5}, 50.0),
                make_sphere({-3429.5, 215.5, -1886.5}, 40.0),
                make_sphere({-3382, 206, -1805}, 35.0),
                make_sphere({-3408, 208.672, -1845}, 40.0),
                make_sphere({-3522.5, 230, -1825}, 40.0)
            }
        },
        {
            "Gerudo Town",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3844.5, 149.169, 2926.197},
            134.537,
            {make_box({-3844.5, 149.169, 2926.197}, {100, 50, 90}, 0.785398)}
        },
        {
            "Gerudo Shelter",
            "https://cdn.wikimg.net/en/zeldawiki/images/4/44/TotK_Gerudo_Shelter.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3866, 116.655, 2942},
            113.137,
            {
                make_box({-3866, 116.655, 2942}, {80, 16, 80}, 0.785398),
                make_box({-3858.96, 138.3, 2940.523}, {17.5, 8.2, 5}, 0.785398)
            }
        },
        {
            "Kara Kara Bazaar",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Kara_Kara_Bazaar.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3239.28, 72.855, 2569.85},
            67.501,
            {make_sphere({-3239.28, 72.855, 2569.85}, 67.501)}
        },
        {
            "Korok Forest",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {433.816, 118.455, -2208.076},
            150.0,
            {make_sphere({433.816, 118.455, -2208.076}, 150.0)}
        },
        {
            "Goron City",
            "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1680, 450, -2445},
            170.0,
            {
                make_cylinder({1680, 450, -2445}, 100.0, 45.0),
                make_cylinder({1734.077, 469.109, -2547.36}, 55.168, 55.168)
            }
        },
        {
            "YunoboCo HQ",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/55/TotK_YunoboCo_HQ.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1618.5, 420, -2858.5},
            85.0,
            {make_sphere({1618.5, 420, -2858.5}, 85.0)}
        },
        {
            "Southern Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/c/cf/TotK_Southern_Mine.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1657.886, 378.497, -1973.852},
            210.0,
            {
                make_sphere({1657.886, 378.497, -1973.852}, 60.0),
                make_box({1800.955, 425.915, -1983.352}, {7, 5, 2}, -0.872665)
            }
        },
        {
            "Bedrock Bistro",
            "https://cdn.wikimg.net/en/zeldawiki/images/1/14/TotK_Bedrock_Bistro.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1730.5, 327.5, -1539},
            55.0,
            {make_sphere({1730.5, 327.5, -1539}, 55.0)}
        },
        {
            "Kakariko Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/af/TotK_Kakariko_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1852.767, 117.803, 987.262},
            220.0,
            {
                make_sphere({1852.767, 117.803, 987.262}, 110.0),
                make_cylinder({1925.783, 264.368, 1067.8}, 32.0, 35.23),
                make_box({1865.47, 248.807, 1051.585}, {53.755, 44.65, 45.289}, 0.274758),
                make_cylinder({1710.002, 215.753, 930.023}, 70.0, 101.232),
                make_box({1860.055, 244.304, 882.548}, {56.571, 40.942, 98.761}, -0.161432),
                make_box({1792.209, 214.726, 876.363}, {30, 45, 60}, 0.456777)
            }
        },
        {
            "Lurelin Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {2990, -85, 3620},
            400.0,
            {
                make_sphere({2868.801, -74.896, 3450.498}, 175.0),
                make_sphere({3035.962, -99.205, 3656.41}, 150.0),
                make_sphere({3037.204, -99.205, 3787.015}, 150.0)
            }
        },
        {
            "Hudson Construction Site",
            "https://cdn.wikimg.net/en/zeldawiki/images/3/35/TotK_Hudson_Construction_Site.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3660, 34.054, -1695},
            241.0,
            {
                make_cylinder({3685.655, 34.054, -1618.478}, 160.0, 160.0),
                make_cylinder({3635.684, 34.054, -1769.108}, 140.0, 140.0)
            }
        },
        {
            "Zora's Domain",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3319.414, 215.431, -503.831},
            164.0,
            {make_cylinder({3319.414, 215.431, -503.831}, 164.0, 90.0)}
        },
        {
            "Hateno Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3470, 115, 2110},
            320.0,
            {
                make_sphere({3379.61, 44.254, 2161.608}, 140.0),
                make_sphere({3598.36, 172.516, 2137.678}, 80.0),
                make_sphere({3337.337, 95.453, 2233.17}, 120.0),
                make_sphere({3478.329, 99.508, 2143.116}, 100.0),
                make_sphere({3385.911, 136.658, 1988.255}, 105.0),
                make_sphere({3547.945, 166.363, 2010.239}, 115.0)
            }
        },
        {
            "Yiga Clan Hideout",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/8d/TotK_Yiga_Clan_Hideout.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3615, 433.206, 1347},
            140.0,
            {
                make_box({-3571.081, 433.206, 1343.792}, {75.335, 20, 60.97}, -1.221364),
                make_box({-3662.441, 433.206, 1351.482}, {34.418, 17.971, 39.394}, -1.048046)
            }
        },
        {
            "Lucky Clover Gazette",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b7/TotK_Lucky_Clover_Gazette.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3269, 184.466, -1776},
            65.0,
            {
                make_sphere({-3255.571, 184.466, -1757.626}, 40.0),
                make_sphere({-3282.353, 184.466, -1794.933}, 40.0),
                make_cylinder({-3269, 184.466, -1776}, 65.0, 50.0)
            }
        },

        // Stables & Mini Stables
        {
            "Dueling Peaks Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1772.954, 93.539, 1948.395},
            40.0,
            {make_sphere({1772.954, 93.539, 1948.395}, 40.0)}
        },
        {
            "Tabantha Bridge Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/TotK_Tabantha_Bridge_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-2948.018, 252.75, -566.453},
            40.0,
            {make_sphere({-2948.018, 252.75, -566.453}, 40.0)}
        },
        {
            "Woodland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/99/TotK_Woodland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1089.235, 105.531, -1149.999},
            40.0,
            {make_sphere({1089.235, 105.531, -1149.999}, 40.0)}
        },
        {
            "South Akkala Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/bd/TotK_South_Akkala_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {3141.677, 247.589, -1681.689},
            80.0,
            {make_sphere({3141.677, 247.589, -1681.689}, 80.0)}
        },
        {
            "Foothill Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Foothill_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {2638.327, 231.324, -1144.679},
            40.0,
            {make_sphere({2638.327, 231.324, -1144.679}, 40.0)}
        },
        {
            "Snowfield Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/98/TotK_Snowfield_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1665.446, 317.253, -2594.84},
            40.0,
            {make_sphere({-1665.446, 317.253, -2594.84}, 40.0)}
        },
        {
            "East Akkala Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/TotK_East_Akkala_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {4226.553, 209.36, -2774.13},
            40.0,
            {make_sphere({4226.553, 209.36, -2774.13}, 40.0)}
        },
        {
            "Wetland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Wetland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {860, 105, 162},
            70.0,
            {
                make_sphere({871.832, 109.254, 192.717}, 40.0),
                make_sphere({846.648, 99.817, 130.265}, 35.0)
            }
        },
        {
            "Outskirt Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_Outskirt_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1440, 98, 1263},
            65.0,
            {
                make_sphere({-1463.216, 97.821, 1279.694}, 37.0),
                make_sphere({-1417.512, 88.503, 1272.431}, 49.0),
                make_sphere({-1449.102, 108.361, 1245.334}, 35.0)
            }
        },
        {
            "Riverside Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Riverside_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {342.385, 62.511, 1120.237},
            60.0,
            {make_sphere({342.385, 62.511, 1120.237}, 60.0)}
        },
        {
            "New Serenne Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_New_Serenne_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1362.113, 191.532, -723.073},
            42.427,
            {make_box({-1362.113, 191.532, -723.073}, {30, 30, 30}, 0.725743)}
        },
        {
            "Gerudo Canyon Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Gerudo_Canyon_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-2800.712, 112.917, 2200.935},
            35.0,
            {make_sphere({-2800.712, 112.917, 2200.935}, 35.0)}
        },
        {
            "Highland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {520.13, 130.89, 3473.016},
            64.0,
            {make_sphere({520.13, 130.89, 3473.016}, 64.0)}
        },
        {
            "Lakeside Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/e/e1/TotK_Lakeside_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1522.445, 135.146, 3538.08},
            60.0,
            {make_sphere({1522.445, 135.146, 3538.08}, 60.0)}
        },

        // Skyview Towers
        {
            "Lookout Landing Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Lookout_Landing_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-298.85, 123.941, -142.85},
            35.0,
            {make_cylinder({-298.85, 123.941, -142.85}, 35.0, 50.0)}
        },
        {
            "Lindor's Brow Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-1909.588, 395.706, -1245.305},
            35.0,
            {make_cylinder({-1909.588, 395.706, -1245.305}, 35.0, 50.0)}
        },
        {
            "Pikida Stonegrove Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/4/4f/TotK_Pikida_Stonegrove_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-2311.495, 542, -3062.495},
            35.0,
            {make_cylinder({-2311.495, 542, -3062.495}, 35.0, 50.0)}
        },
        {
            "Eldin Canyon Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Eldin_Canyon_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {1641.805, 324.418, -1190.82},
            35.0,
            {make_cylinder({1641.805, 324.418, -1190.82}, 35.0, 50.0)}
        },
        {
            "Ulri Mountain Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/TotK_Ulri_Mountain_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {3499, 286.859, -2026},
            35.0,
            {make_cylinder({3499, 286.859, -2026}, 35.0, 50.0)}
        },
        {
            "Sahasra Slope Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/82/TotK_Sahasra_Slope_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {1341.109, 265.256, 1177.858},
            35.0,
            {make_cylinder({1341.109, 265.256, 1177.858}, 35.0, 50.0)}
        },
        {
            "Upland Zorana Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/6/66/TotK_Upland_Zorana_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {2866.062, 478.331, -581.192},
            35.0,
            {make_cylinder({2866.062, 478.331, -581.192}, 35.0, 50.0)}
        },
        {
            "Hyrule Field Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/2f/TotK_Hyrule_Field_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-761.277, 163.416, 1019.228},
            35.0,
            {make_cylinder({-761.277, 163.416, 1019.228}, 35.0, 50.0)}
        },
        {
            "Gerudo Canyon Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/95/TotK_Gerudo_Canyon_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-2438.851, 405.619, 2182.764},
            35.0,
            {make_cylinder({-2438.851, 405.619, 2182.764}, 35.0, 50.0)}
        },
        {
            "Gerudo Highlands Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/TotK_Gerudo_Highlands_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-3960.877, 521.369, 1305.596},
            35.0,
            {make_cylinder({-3960.877, 521.369, 1305.596}, 35.0, 50.0)}
        },
        {
            "Rabella Wetlands Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/c/c9/TotK_Rabella_Wetlands_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {2420, 321, 2754.891},
            35.0,
            {make_cylinder({2420, 321, 2754.891}, 35.0, 50.0)}
        },
        {
            "Thyphlo Ruins Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/e/e6/TotK_Thyphlo_Ruins_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {343.675, 278.562, -3141.648},
            35.0,
            {make_cylinder({343.675, 278.562, -3141.648}, 35.0, 50.0)}
        },
        {
            "Popla Foothills Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/TotK_Popla_Foothills_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {604.839, 197.333, 2126.876},
            35.0,
            {make_cylinder({604.839, 197.333, 2126.876}, 35.0, 50.0)}
        },
        {
            "Mount Lanayru Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/76/TotK_Mount_Lanayru_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {3847.638, 637.808, 1314.911},
            35.0,
            {make_cylinder({3847.638, 637.808, 1314.911}, 35.0, 50.0)}
        },
        {
            "Rospro Pass Skyview Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/4/45/TotK_Rospro_Pass_Skyview_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-3679.585, 331.739, -2346.404},
            35.0,
            {make_cylinder({-3679.585, 331.739, -2346.404}, 35.0, 50.0)}
        },

        // Depths Abandoned Mines
        {
            "Great Abandoned Central Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Great_Abandoned_Central_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {-770, -510, 1890},
            240.0,
            {
                make_box({-811.155, -583.063, 1922.583}, {60.765, 124.82, 90.129}, -0.785398),
                make_cylinder({-729.122, -444.216, 1859.212}, 80.0, 30.0),
                make_cylinder({-770, -510, 1890}, 240.0, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Hebra Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Abandoned_Hebra_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {-3473.313, -468.548, -1852.229},
            kAbandonedMineRadius,
            {
                make_sphere({-3473.313, -468.548, -1852.229}, 137.92),
                make_cylinder({-3473.313, -468.548, -1852.229}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Gerudo Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_Abandoned_Gerudo_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {-3805.02, -524.426, 2876.482},
            kAbandonedMineRadius,
            {
                make_sphere({-3805.02, -524.426, 2876.482}, 113.373),
                make_cylinder({-3805.02, -524.426, 2876.482}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Kara Kara Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/5d/TotK_Abandoned_Kara_Kara_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {-3213.222, -452.986, 2472.537},
            kAbandonedMineRadius,
            {
                make_sphere({-3213.222, -452.986, 2472.537}, 64.34),
                make_cylinder({-3213.222, -452.986, 2472.537}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Eldin Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/75/TotK_Abandoned_Eldin_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {1644.033, -788.04, -2406.065},
            kAbandonedMineRadius,
            {
                make_sphere({1644.033, -788.04, -2406.065}, 67.761),
                make_cylinder({1644.033, -788.04, -2406.065}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Kakariko Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/93/TotK_Abandoned_Kakariko_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {1834.819, -627.515, 1198.487},
            kAbandonedMineRadius,
            {
                make_sphere({1834.819, -627.515, 1198.487}, 106.132),
                make_cylinder({1834.819, -627.515, 1198.487}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Lurelin Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Abandoned_Lurelin_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {2902.169, -436.025, 3381.745},
            kAbandonedMineRadius,
            {
                make_sphere({2902.169, -436.025, 3381.745}, 88.2),
                make_cylinder({2902.169, -436.025, 3381.745}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Tarrey Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/d/d4/TotK_Abandoned_Tarrey_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {3968.37, -542.007, -1610.529},
            kAbandonedMineRadius,
            {
                make_sphere({3968.37, -542.007, -1610.529}, 72.491),
                make_cylinder({3968.37, -542.007, -1610.529}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Lanayru Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/0b/TotK_Abandoned_Lanayru_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {3281.13, -723.967, -563.565},
            kAbandonedMineRadius,
            {
                make_sphere({3281.13, -723.967, -563.565}, 74.827),
                make_cylinder({3281.13, -723.967, -563.565}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },
        {
            "Abandoned Hateno Mine",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/2b/TotK_Abandoned_Hateno_Mine.png",
            "AbandonedMine",
            ZeldaNotesLayer::Underground,
            {3567.668, -601.87, 2238.528},
            kAbandonedMineRadius,
            {
                make_sphere({3567.668, -601.87, 2238.528}, 89.029),
                make_cylinder({3567.668, -601.87, 2238.528}, kAbandonedMineRadius, kAbandonedMineHalfHeight)
            }
        },

        // Sky Archipelagos
        {
            "Tabantha Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/80/TotK_Tabantha_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {-3676.139, 1339.59, -664.48},
            707.0,
            {make_box({-3676.139, 1339.59, -664.48}, {500, 400, 500}, 0.0)}
        },
        {
            "North Gerudo Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_North_Gerudo_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {-3580.118, 1567, 526.512},
            700.0,
            {make_cylinder({-3580.118, 1567, 526.512}, 700.0, 700.0)}
        },
        {
            "West Hyrule Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/c/c8/TotK_West_Hyrule_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {-2290.425, 850, 411.736},
            212.0,
            {make_box({-2290.425, 850, 411.736}, {150, 150, 150}, 0.0)}
        },
        {
            "South Hyrule Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f9/TotK_South_Hyrule_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {-992.385, 812.406, 1445.262},
            750.0,
            {make_box({-992.385, 812.406, 1445.262}, {600, 400, 450}, 0.872665)}
        },
        {
            "Faron Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/71/TotK_Faron_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {-112.51, 625.387, 2550.025},
            492.0,
            {make_box({-112.51, 625.387, 2550.025}, {450, 300, 200}, 0.199866)}
        },
        {
            "Central Hyrule Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/97/TotK_Central_Hyrule_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {246.048, 332.799, -69.875},
            1662.0,
            {make_box({246.048, 332.799, -69.875}, {1600, 150, 450}, 0.0)}
        },
        {
            "Lanayru Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Lanayru_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {3043.103, 859.863, -114.663},
            646.0,
            {make_box({3043.103, 859.863, -114.663}, {240, 200, 600}, 0.0)}
        },
        {
            "Sokkala Sky Archipelago",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/2a/TotK_Sokkala_Sky_Archipelago.png",
            "SkyArchipelago",
            ZeldaNotesLayer::Sky,
            {3755.639, 1022.561, -1675.351},
            500.0,
            {make_box({3755.639, 1022.561, -1675.351}, {300, 300, 400.003}, 0.334117)}
        }
    };
    return locations;
}

// ============================================================================
// Breath of the Wild Location Definitions
// ============================================================================

const std::vector<LocationDefinition>& get_botw_locations() {
    static const std::vector<LocationDefinition> locations = {
        // Settlements & Villages
        {
            "Rito Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/26/BotW_Rito_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3620.765, 288.672, -1799.816},
            80.0,
            {make_sphere({-3620.765, 288.672, -1799.816}, 80.0)}
        },
        {
            "Gerudo Town",
            "https://cdn.wikimg.net/en/zeldawiki/images/0/03/BotW_Gerudo_Town.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3844.5, 194.5, 2925},
            134.537,
            {make_box({-3844.5, 194.5, 2925}, {100, 50, 90}, 0.785398)}
        },
        {
            "Kara Kara Bazaar",
            "https://cdn.wikimg.net/en/zeldawiki/images/c/cb/BotW_Kara_Kara_Bazaar.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {-3239.28, 140.356, 2569.85},
            67.501,
            {make_sphere({-3239.28, 140.356, 2569.85}, 67.501)}
        },
        {
            "Korok Forest",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/BotW_Korok_Forest.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {433.816, 268.455, -2208.076},
            150.0,
            {make_sphere({433.816, 268.455, -2208.076}, 150.0)}
        },
        {
            "Goron City",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/5c/BotW_Goron_City.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1680, 495, -2445},
            100.0,
            {make_cylinder({1680, 495, -2445}, 100.0, 45.0)}
        },
        {
            "Kakariko Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/BotW_Kakariko_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {1852.767, 227.803, 987.262},
            110.0,
            {make_sphere({1852.767, 227.803, 987.262}, 110.0)}
        },
        {
            "Lurelin Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/BotW_Lurelin_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {2868.801, 100.104, 3450.498},
            175.0,
            {make_sphere({2868.801, 100.104, 3450.498}, 175.0)}
        },
        {
            "Tarrey Town",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/99/BotW_Tarrey_Town.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3964.299, 235.622, -1612.209},
            65.0,
            {make_sphere({3964.299, 235.622, -1612.209}, 65.0)}
        },
        {
            "Zora's Domain",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f5/BotW_Zora%27s_Domain.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3321.546, 241.756, -502.415},
            150.0,
            {make_sphere({3321.546, 241.756, -502.415}, 150.0)}
        },
        {
            "Hateno Village",
            "https://cdn.wikimg.net/en/zeldawiki/images/1/10/BotW_Hateno_Village.png",
            "Village",
            ZeldaNotesLayer::Ground,
            {3515, 230, 2145},
            320.0,
            {
                make_sphere({3683.176, 239.097, 2079.311}, 139.152),
                make_sphere({3379.61, 225.453, 2176.045}, 130.0),
                make_sphere({3337.337, 215.453, 2233.17}, 120.0),
                make_sphere({3598.36, 252.516, 2137.678}, 80.0),
                make_sphere({3478.329, 199.508, 2143.116}, 100.0)
            }
        },

        // Stables
        {
            "Foothill Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/58/BotW_Foothill_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {2613.332, 253.358, -1143.513},
            40.0,
            {make_sphere({2613.332, 253.358, -1143.513}, 40.0)}
        },
        {
            "Dueling Peaks Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/BotW_Dueling_Peaks_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1761.314, 115.573, 1926.244},
            40.0,
            {make_sphere({1761.314, 115.573, 1926.244}, 40.0)}
        },
        {
            "Lakeside Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/a3/BotW_Lakeside_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1552.023, 166.02, 3537.839},
            40.0,
            {make_sphere({1552.023, 166.02, 3537.839}, 40.0)}
        },
        {
            "Highland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/be/BotW_Highland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {529.6, 152.925, 3450.883},
            40.0,
            {make_sphere({529.6, 152.925, 3450.883}, 40.0)}
        },
        {
            "Woodland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/BotW_Woodland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {1065.67, 127.565, -1141.583},
            40.0,
            {make_sphere({1065.67, 127.565, -1141.583}, 40.0)}
        },
        {
            "Gerudo Canyon Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f2/BotW_Gerudo_Canyon_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-2803.812, 134.951, 2225.765},
            40.0,
            {make_sphere({-2803.812, 134.951, 2225.765}, 40.0)}
        },
        {
            "Outskirt Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/bc/BotW_Outskirt_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1449.493, 137.751, 1269.011},
            40.0,
            {make_sphere({-1449.493, 137.751, 1269.011}, 40.0)}
        },
        {
            "Serenne Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/BotW_Serenne_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1559.375, 212.325, -1799.053},
            40.0,
            {make_sphere({-1559.375, 212.325, -1799.053}, 40.0)}
        },
        {
            "Wetland Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/1/11/BotW_Wetland_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {888.062, 131.289, 173.672},
            40.0,
            {make_sphere({888.062, 131.289, 173.672}, 40.0)}
        },
        {
            "Rito Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/87/BotW_Rito_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-3255.571, 224.466, -1757.625},
            40.0,
            {make_sphere({-3255.571, 224.466, -1757.625}, 40.0)}
        },
        {
            "Riverside Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/6/65/BotW_Riverside_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {339.232, 115.312, 1095.297},
            40.0,
            {make_sphere({339.232, 115.312, 1095.297}, 40.0)}
        },
        {
            "Tabantha Bridge Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/1/15/BotW_Tabantha_Bridge_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-2931.568, 274.784, -547.598},
            40.0,
            {make_sphere({-2931.568, 274.784, -547.598}, 40.0)}
        },
        {
            "Snowfield Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/5a/BotW_Snowfield_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {-1654.883, 339.288, -2572.156},
            40.0,
            {make_sphere({-1654.883, 339.288, -2572.156}, 40.0)}
        },
        {
            "East Akkala Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/fd/BotW_East_Akkala_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {4227.582, 231.441, -2749.128},
            40.0,
            {make_sphere({4227.582, 231.441, -2749.128}, 40.0)}
        },
        {
            "South Akkala Stable",
            "https://cdn.wikimg.net/en/zeldawiki/images/9/95/BotW_South_Akkala_Stable.png",
            "Stable",
            ZeldaNotesLayer::Ground,
            {3149.832, 306.78, -1692.647},
            40.0,
            {make_sphere({3149.832, 306.78, -1692.647}, 40.0)}
        },

        // Sheikah Towers
        {
            "Hebra Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/a/ac/BotW_Hebra_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-2173, 489.537, -2034},
            kBotwTowerRadius,
            {make_capsule({-2173, 489.537, -2034}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Tabantha Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/8e/BotW_Tabantha_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-3613.748, 405.392, -990.165},
            kBotwTowerRadius,
            {make_capsule({-3613.748, 405.392, -990.165}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Gerudo Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/71/BotW_Gerudo_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-3666, 430.656, 1828.6},
            kBotwTowerRadius,
            {make_capsule({-3666, 430.656, 1828.6}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Wasteland Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/8/8c/BotW_Wasteland_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-2306.836, 490.537, 2437.32},
            kBotwTowerRadius,
            {make_capsule({-2306.836, 490.537, 2437.32}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Woodland Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/7e/BotW_Woodland_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {883.884, 310.571, -1605.71},
            kBotwTowerRadius,
            {make_capsule({883.884, 310.571, -1605.71}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Central Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/BotW_Central_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-788.645, 157.998, 442.031},
            kBotwTowerRadius,
            {make_capsule({-788.645, 157.998, 442.031}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Great Plateau Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/2/27/BotW_Great_Plateau_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-560.035, 206.777, 1694.863},
            kBotwTowerRadius,
            {make_capsule({-560.035, 206.777, 1694.863}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Dueling Peaks Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/b/b4/BotW_Dueling_Peaks_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {1016.777, 144.362, 1714.082},
            kBotwTowerRadius,
            {make_capsule({1016.777, 144.362, 1714.082}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Lake Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/7e/BotW_Lake_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-31.816, 240.835, 2961.601},
            kBotwTowerRadius,
            {make_capsule({-31.816, 240.835, 2961.601}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Eldin Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/3/34/BotW_Eldin_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {2174.151, 469.084, -1556.781},
            kBotwTowerRadius,
            {make_capsule({2174.151, 469.084, -1556.781}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Akkala Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/BotW_Akkala_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {3308, 554.137, -1500.1},
            kBotwTowerRadius,
            {make_capsule({3308, 554.137, -1500.1}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Lanayru Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/7/74/BotW_Lanayru_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {2258, 271.272, -109},
            kBotwTowerRadius,
            {make_capsule({2258, 271.272, -109}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Hateno Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/5/54/BotW_Hateno_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {2735.5, 296.537, 2133.5},
            kBotwTowerRadius,
            {make_capsule({2735.5, 296.537, 2133.5}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Faron Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/3/3c/BotW_Faron_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {1331.203, 230.287, 3273.723},
            kBotwTowerRadius,
            {make_capsule({1331.203, 230.287, 3273.723}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        },
        {
            "Ridgeland Tower",
            "https://cdn.wikimg.net/en/zeldawiki/images/d/d9/BotW_Ridgeland_Tower.png",
            "Tower",
            ZeldaNotesLayer::Ground,
            {-1755.3, 288.937, -774.3},
            kBotwTowerRadius,
            {make_capsule({-1755.3, 288.937, -774.3}, kBotwTowerRadius, kBotwTowerHalfHeight)}
        }
    };
    return locations;
}

}  // namespace

ZeldaLocationResult resolve_totk_location_3d(
    const ZeldaNotesVector3& pos,
    ZeldaNotesLayer layer) {
    auto effective_layer = layer;
    if (effective_layer == ZeldaNotesLayer::Unknown) {
        if (pos.y >= kTotkSkyMinY) {
            effective_layer = ZeldaNotesLayer::Sky;
        } else if (pos.y <= kTotkDepthsMaxY) {
            effective_layer = ZeldaNotesLayer::Underground;
        } else {
            effective_layer = ZeldaNotesLayer::Ground;
        }
    }

    // Gloom's Origin / Abyss Below Hyrule Castle
    if (effective_layer == ZeldaNotesLayer::Underground && pos.y <= -600.0 &&
        pos.x >= -650.0 && pos.x <= 650.0 && pos.z >= -1450.0 && pos.z <= -150.0) {
        ZeldaLocationResult result;
        result.name = "Gloom's Origin";
        result.image_url = "https://cdn.wikimg.net/en/zeldawiki/images/a/a1/TotK_Gloom%27s_Origin.png";
        result.category = "abyss";
        result.layer = ZeldaNotesLayer::Underground;
        result.matched = true;
        return result;
    }

    for (const auto& loc : get_totk_locations()) {
        if (is_within_location(pos, loc, effective_layer)) {
            ZeldaLocationResult result;
            result.name = loc.name;
            result.image_url = loc.image_url;
            result.category = loc.category;
            result.layer = loc.layer;
            result.matched = true;
            return result;
        }
    }

    return {};
}

ZeldaLocationResult resolve_botw_location_3d(const ZeldaNotesVector3& pos) {
    for (const auto& loc : get_botw_locations()) {
        if (is_within_location(pos, loc, ZeldaNotesLayer::Ground)) {
            ZeldaLocationResult result;
            result.name = loc.name;
            result.image_url = loc.image_url;
            result.category = loc.category;
            result.layer = loc.layer;
            result.matched = true;
            return result;
        }
    }

    return {};
}

std::string resolve_poi_artwork(
    const std::string& poi_name,
    ZeldaNotesGame game) {
    if (poi_name.empty()) return {};

    if (game == ZeldaNotesGame::TearsOfTheKingdom) {
        // Gloom / Final Boss Abyss
        if (poi_name.find("Gloom") != std::string::npos || poi_name.find("Demon King") != std::string::npos) {
            return "https://cdn.wikimg.net/en/zeldawiki/images/a/a1/TotK_Gloom%27s_Origin.png";
        }

        // Settlements & Villages
        if (poi_name.find("Tarrey Town") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png";
        if (poi_name.find("Lookout Landing") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/94/TotK_Lookout_Landing.png";
        if (poi_name.find("Hateno Village") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png";
        if (poi_name.find("Kakariko Village") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/a/af/TotK_Kakariko_Village.png";
        if (poi_name.find("Goron City") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png";
        if (poi_name.find("Rito Village") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png";
        if (poi_name.find("Zora's Domain") != std::string::npos || poi_name.find("Zoras Domain") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png";
        if (poi_name.find("Gerudo Town") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png";
        if (poi_name.find("Kara Kara") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Kara_Kara_Bazaar.png";
        if (poi_name.find("Lurelin") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png";
        if (poi_name.find("Korok Forest") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png";
        if (poi_name.find("Hudson Construction") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/3/35/TotK_Hudson_Construction_Site.png";
        if (poi_name.find("YunoboCo") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/5/55/TotK_YunoboCo_HQ.png";
        if (poi_name.find("Bedrock Bistro") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/1/14/TotK_Bedrock_Bistro.png";
        if (poi_name.find("Lucky Clover") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/b/b7/TotK_Lucky_Clover_Gazette.png";
        if (poi_name.find("Flight Range") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/3/31/TotK_Flight_Range.png";
        if (poi_name.find("Forgotten Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/3/33/TotK_Forgotten_Temple.png";
        if (poi_name.find("Hyrule Castle") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Hyrule_Castle.png";
        if (poi_name.find("Temple of Time") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/e/e4/TotK_Temple_of_Time.png";
        if (poi_name.find("Yiga Clan") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/8d/TotK_Yiga_Clan_Hideout.png";

        // Stables
        if (poi_name.find("East Akkala Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/TotK_East_Akkala_Stable.png";
        if (poi_name.find("South Akkala Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/b/bd/TotK_South_Akkala_Stable.png";
        if (poi_name.find("Dueling Peaks Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png";
        if (poi_name.find("Tabantha Bridge Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/TotK_Tabantha_Bridge_Stable.png";
        if (poi_name.find("Woodland Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/99/TotK_Woodland_Stable.png";
        if (poi_name.find("Foothill Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Foothill_Stable.png";
        if (poi_name.find("Snowfield Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/98/TotK_Snowfield_Stable.png";
        if (poi_name.find("Wetland Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Wetland_Stable.png";
        if (poi_name.find("Outskirt Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_Outskirt_Stable.png";
        if (poi_name.find("Riverside Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Riverside_Stable.png";
        if (poi_name.find("New Serenne Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_New_Serenne_Stable.png";
        if (poi_name.find("Gerudo Canyon Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Gerudo_Canyon_Stable.png";
        if (poi_name.find("Highland Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png";
        if (poi_name.find("Lakeside Stable") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/e/e1/TotK_Lakeside_Stable.png";

        // Skyview Towers
        if (poi_name.find("Ulri Mountain") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/TotK_Ulri_Mountain_Skyview_Tower.png";
        if (poi_name.find("Lindor's Brow") != std::string::npos || poi_name.find("Lindors Brow") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png";
        if (poi_name.find("Pikida Stonegrove") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/4/4f/TotK_Pikida_Stonegrove_Skyview_Tower.png";
        if (poi_name.find("Eldin Canyon") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Eldin_Canyon_Skyview_Tower.png";
        if (poi_name.find("Sahasra Slope") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/82/TotK_Sahasra_Slope_Skyview_Tower.png";
        if (poi_name.find("Upland Zorana") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/6/66/TotK_Upland_Zorana_Skyview_Tower.png";
        if (poi_name.find("Popla Foothills") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/TotK_Popla_Foothills_Skyview_Tower.png";
        if (poi_name.find("Rabella Wetlands") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/c/c9/TotK_Rabella_Wetlands_Skyview_Tower.png";
        if (poi_name.find("Gerudo Canyon Tower") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/95/TotK_Gerudo_Canyon_Skyview_Tower.png";
        if (poi_name.find("Gerudo Highlands Tower") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/TotK_Gerudo_Highlands_Skyview_Tower.png";
        if (poi_name.find("Thyphlo Ruins Tower") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/e/e6/TotK_Thyphlo_Ruins_Skyview_Tower.png";
        if (poi_name.find("Mount Lanayru Tower") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/76/TotK_Mount_Lanayru_Skyview_Tower.png";
        if (poi_name.find("Rospro Pass") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/4/45/TotK_Rospro_Pass_Skyview_Tower.png";
        if (poi_name.find("Hyrule Field Tower") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/2/2f/TotK_Hyrule_Field_Skyview_Tower.png";

        // Sky Archipelagos & Temples
        if (poi_name.find("Great Sky Island") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/6/6c/TotK_Great_Sky_Island.png";
        if (poi_name.find("Sokkala") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/2/2a/TotK_Sokkala_Sky_Archipelago.png";
        if (poi_name.find("Dragonhead") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/TotK_Dragonhead_Island.png";
        if (poi_name.find("Starview") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/4/42/TotK_Starview_Island.png";
        if (poi_name.find("Wind Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/d/db/TotK_Wind_Temple_B2.png";
        if (poi_name.find("Water Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/96/TotK_Water_Temple_1F.png";
        if (poi_name.find("Fire Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/b/bc/TotK_Fire_Temple_2F.png";
        if (poi_name.find("Lightning Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/81/TotK_Lightning_Temple_7F.png";
        if (poi_name.find("Spirit Temple") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Spirit_Temple_Interior.png";
        if (poi_name.find("Lanayru Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Lanayru_Sky_Archipelago.png";
        if (poi_name.find("Tabantha Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/8/80/TotK_Tabantha_Sky_Archipelago.png";
        if (poi_name.find("North Gerudo Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_North_Gerudo_Sky_Archipelago.png";
        if (poi_name.find("South Hyrule Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/f/f9/TotK_South_Hyrule_Sky_Archipelago.png";
        if (poi_name.find("Central Hyrule Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/97/TotK_Central_Hyrule_Sky_Archipelago.png";
        if (poi_name.find("West Hyrule Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/c/c8/TotK_West_Hyrule_Sky_Archipelago.png";
        if (poi_name.find("Faron Sky") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/71/TotK_Faron_Sky_Archipelago.png";

        // Depths Mines
        if (poi_name.find("Central Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Great_Abandoned_Central_Mine.png";
        if (poi_name.find("Tarrey Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/d/d4/TotK_Abandoned_Tarrey_Mine.png";
        if (poi_name.find("Lanayru Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/0b/TotK_Abandoned_Lanayru_Mine.png";
        if (poi_name.find("Gerudo Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_Abandoned_Gerudo_Mine.png";
        if (poi_name.find("Hebra Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Abandoned_Hebra_Mine.png";
        if (poi_name.find("Eldin Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/7/75/TotK_Abandoned_Eldin_Mine.png";
        if (poi_name.find("Kakariko Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/9/93/TotK_Abandoned_Kakariko_Mine.png";
        if (poi_name.find("Hateno Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/2/2b/TotK_Abandoned_Hateno_Mine.png";
        if (poi_name.find("Lurelin Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Abandoned_Lurelin_Mine.png";
        if (poi_name.find("Kara Kara Mine") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/5/5d/TotK_Abandoned_Kara_Kara_Mine.png";
        if (poi_name.find("Construct Factory") != std::string::npos) return "https://cdn.wikimg.net/en/zeldawiki/images/e/e9/TotK_Construct_Factory.png";
    }

    return {};
}

std::string resolve_zelda_region_artwork(
    const std::string& region,
    ZeldaNotesGame game,
    ZeldaNotesLayer layer) {
    if (game == ZeldaNotesGame::TearsOfTheKingdom) {
        if (layer == ZeldaNotesLayer::Sky) {
            return "https://cdn.wikimg.net/en/zeldawiki/images/6/6c/TotK_Great_Sky_Island.png";
        }
        if (layer == ZeldaNotesLayer::Underground) {
            return "https://cdn.wikimg.net/en/zeldawiki/images/1/17/TotK_Depths_Promotional_Screenshot.jpg";
        }
        if (region == "Akkala") return "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png";
        if (region == "Central Hyrule" || region == "Hyrule Field") return "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Hyrule_Castle.png";
        if (region == "Eldin") return "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png";
        if (region == "Gerudo") return "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png";
        if (region == "Hebra") return "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png";
        if (region == "Lanayru") return "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png";
        if (region == "Necluda") return "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png";
        if (region == "Faron") return "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png";
        if (region == "Great Hyrule Forest") return "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png";
        if (region == "Hyrule Ridge") return "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png";
        if (region == "Dueling Peaks") return "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png";
        if (region == "Lake Hylia") return "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png";
    } else if (game == ZeldaNotesGame::BreathOfTheWild) {
        if (region == "Akkala") return "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/BotW_Akkala_Tower.png";
        if (region == "Central Hyrule") return "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/BotW_Central_Tower.png";
        if (region == "Eldin") return "https://cdn.wikimg.net/en/zeldawiki/images/5/5c/BotW_Goron_City.png";
        if (region == "Gerudo") return "https://cdn.wikimg.net/en/zeldawiki/images/0/03/BotW_Gerudo_Town.png";
        if (region == "Hebra") return "https://cdn.wikimg.net/en/zeldawiki/images/2/26/BotW_Rito_Village.png";
        if (region == "Lanayru") return "https://cdn.wikimg.net/en/zeldawiki/images/f/f5/BotW_Zora%27s_Domain.png";
        if (region == "Necluda") return "https://cdn.wikimg.net/en/zeldawiki/images/1/10/BotW_Hateno_Village.png";
        if (region == "Faron") return "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/BotW_Lurelin_Village.png";
        if (region == "Great Hyrule Forest") return "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/BotW_Korok_Forest.png";
    }
    return {};
}

}  // namespace nso
