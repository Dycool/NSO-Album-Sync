//! Pure Zelda Notes 3D geometry and artwork helpers.
//!
//! This is a direct safe-Rust port of the C++ location definitions. Keep the
//! coordinates, radii, shape order and artwork mappings in lockstep with the
//! original implementation; the order is observable when areas overlap.

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum ZeldaGame {
    #[default]
    Unknown,
    BreathOfTheWild,
    TearsOfTheKingdom,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum ZeldaLayer {
    #[default]
    Unknown,
    Ground,
    Sky,
    Underground,
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Vector3 {
    x: f64,
    y: f64,
    z: f64,
}

impl Vector3 {
    pub fn new(x: f64, y: f64, z: f64) -> Option<Self> {
        (x.is_finite() && y.is_finite() && z.is_finite()).then_some(Self { x, y, z })
    }

    pub fn x(self) -> f64 { self.x }
    pub fn y(self) -> f64 { self.y }
    pub fn z(self) -> f64 { self.z }
}

#[derive(Debug, Clone, Default)]
pub struct LocationResult {
    name: String,
    image_url: String,
    category: String,
    layer: ZeldaLayer,
    matched: bool,
}

impl LocationResult {
    pub fn name(&self) -> &str { &self.name }
    pub fn image_url(&self) -> &str { &self.image_url }
    pub fn category(&self) -> &str { &self.category }
    pub fn layer(&self) -> ZeldaLayer { self.layer }
    pub fn matched(&self) -> bool { self.matched }
}

#[derive(Clone, Copy)]
enum Shape {
    Sphere { center: (f64, f64, f64), radius: f64 },
    Box { center: (f64, f64, f64), half: (f64, f64, f64), rotation_y: f64 },
    Cylinder { center: (f64, f64, f64), radius: f64, half_height: f64 },
    Capsule { center: (f64, f64, f64), radius: f64, half_height: f64 },
}

#[derive(Clone, Copy)]
struct Location {
    name: &'static str,
    image: &'static str,
    category: &'static str,
    layer: ZeldaLayer,
    center: (f64, f64, f64),
    bounding_radius: f64,
    shapes: &'static [Shape],
}

const TOTK_SKY_MIN_Y: f64 = 500.0;
const TOTK_DEPTHS_MAX_Y: f64 = -300.0;
const ABANDONED_MINE_RADIUS: f64 = 200.0;
const ABANDONED_MINE_HALF_HEIGHT: f64 = 180.0;
const BOTW_TOWER_RADIUS: f64 = 25.0;
const BOTW_TOWER_HALF_HEIGHT: f64 = 34.537;

const TOTK_LOCATIONS: &[Location] = &[
    // Settlements & Villages
    Location { name: "Lookout Landing", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/94/TotK_Lookout_Landing.png", category: "Village", layer: ZeldaLayer::Ground, center: (-254.12, 123.447, -101.602), bounding_radius: 83.45, shapes: &[Shape::Box { center: (-254.12, 123.447, -101.602), half: (55.012, 16.807, 62.732), rotation_y: 0.0 }] },
    Location { name: "Tarrey Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png", category: "Village", layer: ZeldaLayer::Ground, center: (3964.299, 152.472, -1612.209), bounding_radius: 85.0, shapes: &[Shape::Sphere { center: (3964.299, 152.472, -1612.209), radius: 85.0 }] },
    Location { name: "Rito Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3501.0, 215.0, -1835.0), bounding_radius: 210.0, shapes: &[
        Shape::Sphere { center: (-3620.765, 208.672, -1799.816), radius: 80.0 },
        Shape::Sphere { center: (-3482.0, 208.672, -1841.5), radius: 50.0 },
        Shape::Sphere { center: (-3429.5, 215.5, -1886.5), radius: 40.0 },
        Shape::Sphere { center: (-3382.0, 206.0, -1805.0), radius: 35.0 },
        Shape::Sphere { center: (-3408.0, 208.672, -1845.0), radius: 40.0 },
        Shape::Sphere { center: (-3522.5, 230.0, -1825.0), radius: 40.0 },
    ] },
    Location { name: "Gerudo Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3844.5, 149.169, 2926.197), bounding_radius: 134.537, shapes: &[Shape::Box { center: (-3844.5, 149.169, 2926.197), half: (100.0, 50.0, 90.0), rotation_y: 0.785398 }] },
    Location { name: "Gerudo Shelter", image: "https://cdn.wikimg.net/en/zeldawiki/images/4/44/TotK_Gerudo_Shelter.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3866.0, 116.655, 2942.0), bounding_radius: 113.137, shapes: &[
        Shape::Box { center: (-3866.0, 116.655, 2942.0), half: (80.0, 16.0, 80.0), rotation_y: 0.785398 },
        Shape::Box { center: (-3858.96, 138.3, 2940.523), half: (17.5, 8.2, 5.0), rotation_y: 0.785398 },
    ] },
    Location { name: "Kara Kara Bazaar", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Kara_Kara_Bazaar.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3239.28, 72.855, 2569.85), bounding_radius: 67.501, shapes: &[Shape::Sphere { center: (-3239.28, 72.855, 2569.85), radius: 67.501 }] },
    Location { name: "Korok Forest", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png", category: "Village", layer: ZeldaLayer::Ground, center: (433.816, 118.455, -2208.076), bounding_radius: 150.0, shapes: &[Shape::Sphere { center: (433.816, 118.455, -2208.076), radius: 150.0 }] },
    Location { name: "Goron City", image: "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png", category: "Village", layer: ZeldaLayer::Ground, center: (1680.0, 450.0, -2445.0), bounding_radius: 170.0, shapes: &[
        Shape::Cylinder { center: (1680.0, 450.0, -2445.0), radius: 100.0, half_height: 45.0 },
        Shape::Cylinder { center: (1734.077, 469.109, -2547.36), radius: 55.168, half_height: 55.168 },
    ] },
    Location { name: "YunoboCo HQ", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/55/TotK_YunoboCo_HQ.png", category: "Village", layer: ZeldaLayer::Ground, center: (1618.5, 420.0, -2858.5), bounding_radius: 85.0, shapes: &[Shape::Sphere { center: (1618.5, 420.0, -2858.5), radius: 85.0 }] },
    Location { name: "Southern Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/c/cf/TotK_Southern_Mine.png", category: "Village", layer: ZeldaLayer::Ground, center: (1657.886, 378.497, -1973.852), bounding_radius: 210.0, shapes: &[
        Shape::Sphere { center: (1657.886, 378.497, -1973.852), radius: 60.0 },
        Shape::Box { center: (1800.955, 425.915, -1983.352), half: (7.0, 5.0, 2.0), rotation_y: -0.872665 },
    ] },
    Location { name: "Bedrock Bistro", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/14/TotK_Bedrock_Bistro.png", category: "Village", layer: ZeldaLayer::Ground, center: (1730.5, 327.5, -1539.0), bounding_radius: 55.0, shapes: &[Shape::Sphere { center: (1730.5, 327.5, -1539.0), radius: 55.0 }] },
    Location { name: "Kakariko Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/af/TotK_Kakariko_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (1852.767, 117.803, 987.262), bounding_radius: 220.0, shapes: &[
        Shape::Sphere { center: (1852.767, 117.803, 987.262), radius: 110.0 },
        Shape::Cylinder { center: (1925.783, 264.368, 1067.8), radius: 32.0, half_height: 35.23 },
        Shape::Box { center: (1865.47, 248.807, 1051.585), half: (53.755, 44.65, 45.289), rotation_y: 0.274758 },
        Shape::Cylinder { center: (1710.002, 215.753, 930.023), radius: 70.0, half_height: 101.232 },
        Shape::Box { center: (1860.055, 244.304, 882.548), half: (56.571, 40.942, 98.761), rotation_y: -0.161432 },
        Shape::Box { center: (1792.209, 214.726, 876.363), half: (30.0, 45.0, 60.0), rotation_y: 0.456777 },
    ] },
    Location { name: "Lurelin Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (2990.0, -85.0, 3620.0), bounding_radius: 400.0, shapes: &[
        Shape::Sphere { center: (2868.801, -74.896, 3450.498), radius: 175.0 },
        Shape::Sphere { center: (3035.962, -99.205, 3656.41), radius: 150.0 },
        Shape::Sphere { center: (3037.204, -99.205, 3787.015), radius: 150.0 },
    ] },
    Location { name: "Hudson Construction Site", image: "https://cdn.wikimg.net/en/zeldawiki/images/3/35/TotK_Hudson_Construction_Site.png", category: "Village", layer: ZeldaLayer::Ground, center: (3660.0, 34.054, -1695.0), bounding_radius: 241.0, shapes: &[
        Shape::Cylinder { center: (3685.655, 34.054, -1618.478), radius: 160.0, half_height: 160.0 },
        Shape::Cylinder { center: (3635.684, 34.054, -1769.108), radius: 140.0, half_height: 140.0 },
    ] },
    Location { name: "Zora's Domain", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png", category: "Village", layer: ZeldaLayer::Ground, center: (3319.414, 215.431, -503.831), bounding_radius: 164.0, shapes: &[Shape::Cylinder { center: (3319.414, 215.431, -503.831), radius: 164.0, half_height: 90.0 }] },
    Location { name: "Hateno Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (3470.0, 115.0, 2110.0), bounding_radius: 320.0, shapes: &[
        Shape::Sphere { center: (3379.61, 44.254, 2161.608), radius: 140.0 },
        Shape::Sphere { center: (3598.36, 172.516, 2137.678), radius: 80.0 },
        Shape::Sphere { center: (3337.337, 95.453, 2233.17), radius: 120.0 },
        Shape::Sphere { center: (3478.329, 99.508, 2143.116), radius: 100.0 },
        Shape::Sphere { center: (3385.911, 136.658, 1988.255), radius: 105.0 },
        Shape::Sphere { center: (3547.945, 166.363, 2010.239), radius: 115.0 },
    ] },
    Location { name: "Yiga Clan Hideout", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/8d/TotK_Yiga_Clan_Hideout.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3615.0, 433.206, 1347.0), bounding_radius: 140.0, shapes: &[
        Shape::Box { center: (-3571.081, 433.206, 1343.792), half: (75.335, 20.0, 60.97), rotation_y: -1.221364 },
        Shape::Box { center: (-3662.441, 433.206, 1351.482), half: (34.418, 17.971, 39.394), rotation_y: -1.048046 },
    ] },
    Location { name: "Lucky Clover Gazette", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b7/TotK_Lucky_Clover_Gazette.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3269.0, 184.466, -1776.0), bounding_radius: 65.0, shapes: &[
        Shape::Sphere { center: (-3255.571, 184.466, -1757.626), radius: 40.0 },
        Shape::Sphere { center: (-3282.353, 184.466, -1794.933), radius: 40.0 },
        Shape::Cylinder { center: (-3269.0, 184.466, -1776.0), radius: 65.0, half_height: 50.0 },
    ] },

    // Stables & Mini Stables
    Location { name: "Dueling Peaks Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1772.954, 93.539, 1948.395), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (1772.954, 93.539, 1948.395), radius: 40.0 }] },
    Location { name: "Tabantha Bridge Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/TotK_Tabantha_Bridge_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-2948.018, 252.75, -566.453), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-2948.018, 252.75, -566.453), radius: 40.0 }] },
    Location { name: "Woodland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/99/TotK_Woodland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1089.235, 105.531, -1149.999), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (1089.235, 105.531, -1149.999), radius: 40.0 }] },
    Location { name: "South Akkala Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/bd/TotK_South_Akkala_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (3141.677, 247.589, -1681.689), bounding_radius: 80.0, shapes: &[Shape::Sphere { center: (3141.677, 247.589, -1681.689), radius: 80.0 }] },
    Location { name: "Foothill Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Foothill_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (2638.327, 231.324, -1144.679), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (2638.327, 231.324, -1144.679), radius: 40.0 }] },
    Location { name: "Snowfield Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/98/TotK_Snowfield_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1665.446, 317.253, -2594.84), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-1665.446, 317.253, -2594.84), radius: 40.0 }] },
    Location { name: "East Akkala Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/TotK_East_Akkala_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (4226.553, 209.36, -2774.13), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (4226.553, 209.36, -2774.13), radius: 40.0 }] },
    Location { name: "Wetland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Wetland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (860.0, 105.0, 162.0), bounding_radius: 70.0, shapes: &[
        Shape::Sphere { center: (871.832, 109.254, 192.717), radius: 40.0 },
        Shape::Sphere { center: (846.648, 99.817, 130.265), radius: 35.0 },
    ] },
    Location { name: "Outskirt Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_Outskirt_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1440.0, 98.0, 1263.0), bounding_radius: 65.0, shapes: &[
        Shape::Sphere { center: (-1463.216, 97.821, 1279.694), radius: 37.0 },
        Shape::Sphere { center: (-1417.512, 88.503, 1272.431), radius: 49.0 },
        Shape::Sphere { center: (-1449.102, 108.361, 1245.334), radius: 35.0 },
    ] },
    Location { name: "Riverside Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Riverside_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (342.385, 62.511, 1120.237), bounding_radius: 60.0, shapes: &[Shape::Sphere { center: (342.385, 62.511, 1120.237), radius: 60.0 }] },
    Location { name: "New Serenne Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_New_Serenne_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1362.113, 191.532, -723.073), bounding_radius: 42.427, shapes: &[Shape::Box { center: (-1362.113, 191.532, -723.073), half: (30.0, 30.0, 30.0), rotation_y: 0.725743 }] },
    Location { name: "Gerudo Canyon Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Gerudo_Canyon_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-2800.712, 112.917, 2200.935), bounding_radius: 35.0, shapes: &[Shape::Sphere { center: (-2800.712, 112.917, 2200.935), radius: 35.0 }] },
    Location { name: "Highland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (520.13, 130.89, 3473.016), bounding_radius: 64.0, shapes: &[Shape::Sphere { center: (520.13, 130.89, 3473.016), radius: 64.0 }] },
    Location { name: "Lakeside Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/e/e1/TotK_Lakeside_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1522.445, 135.146, 3538.08), bounding_radius: 60.0, shapes: &[Shape::Sphere { center: (1522.445, 135.146, 3538.08), radius: 60.0 }] },

    // Skyview Towers
    Location { name: "Lookout Landing Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Lookout_Landing_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-298.85, 123.941, -142.85), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-298.85, 123.941, -142.85), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Lindor's Brow Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-1909.588, 395.706, -1245.305), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-1909.588, 395.706, -1245.305), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Pikida Stonegrove Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/4/4f/TotK_Pikida_Stonegrove_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-2311.495, 542.0, -3062.495), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-2311.495, 542.0, -3062.495), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Eldin Canyon Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Eldin_Canyon_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (1641.805, 324.418, -1190.82), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (1641.805, 324.418, -1190.82), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Ulri Mountain Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/TotK_Ulri_Mountain_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (3499.0, 286.859, -2026.0), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (3499.0, 286.859, -2026.0), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Sahasra Slope Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/82/TotK_Sahasra_Slope_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (1341.109, 265.256, 1177.858), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (1341.109, 265.256, 1177.858), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Upland Zorana Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/6/66/TotK_Upland_Zorana_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (2866.062, 478.331, -581.192), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (2866.062, 478.331, -581.192), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Hyrule Field Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/2f/TotK_Hyrule_Field_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-761.277, 163.416, 1019.228), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-761.277, 163.416, 1019.228), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Gerudo Canyon Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/95/TotK_Gerudo_Canyon_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-2438.851, 405.619, 2182.764), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-2438.851, 405.619, 2182.764), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Gerudo Highlands Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/TotK_Gerudo_Highlands_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-3960.877, 521.369, 1305.596), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-3960.877, 521.369, 1305.596), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Rabella Wetlands Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/c/c9/TotK_Rabella_Wetlands_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (2420.0, 321.0, 2754.891), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (2420.0, 321.0, 2754.891), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Thyphlo Ruins Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/e/e6/TotK_Thyphlo_Ruins_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (343.675, 278.562, -3141.648), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (343.675, 278.562, -3141.648), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Popla Foothills Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/TotK_Popla_Foothills_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (604.839, 197.333, 2126.876), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (604.839, 197.333, 2126.876), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Mount Lanayru Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/76/TotK_Mount_Lanayru_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (3847.638, 637.808, 1314.911), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (3847.638, 637.808, 1314.911), radius: 35.0, half_height: 50.0 }] },
    Location { name: "Rospro Pass Skyview Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/4/45/TotK_Rospro_Pass_Skyview_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-3679.585, 331.739, -2346.404), bounding_radius: 35.0, shapes: &[Shape::Cylinder { center: (-3679.585, 331.739, -2346.404), radius: 35.0, half_height: 50.0 }] },

    // Depths Abandoned Mines
    Location { name: "Great Abandoned Central Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Great_Abandoned_Central_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (-770.0, -510.0, 1890.0), bounding_radius: 240.0, shapes: &[
        Shape::Box { center: (-811.155, -583.063, 1922.583), half: (60.765, 124.82, 90.129), rotation_y: -0.785398 },
        Shape::Cylinder { center: (-729.122, -444.216, 1859.212), radius: 80.0, half_height: 30.0 },
        Shape::Cylinder { center: (-770.0, -510.0, 1890.0), radius: 240.0, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Hebra Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Abandoned_Hebra_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (-3473.313, -468.548, -1852.229), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (-3473.313, -468.548, -1852.229), radius: 137.92 },
        Shape::Cylinder { center: (-3473.313, -468.548, -1852.229), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Gerudo Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_Abandoned_Gerudo_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (-3805.02, -524.426, 2876.482), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (-3805.02, -524.426, 2876.482), radius: 113.373 },
        Shape::Cylinder { center: (-3805.02, -524.426, 2876.482), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Kara Kara Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/5d/TotK_Abandoned_Kara_Kara_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (-3213.222, -452.986, 2472.537), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (-3213.222, -452.986, 2472.537), radius: 64.34 },
        Shape::Cylinder { center: (-3213.222, -452.986, 2472.537), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Eldin Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/75/TotK_Abandoned_Eldin_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (1644.033, -788.04, -2406.065), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (1644.033, -788.04, -2406.065), radius: 67.761 },
        Shape::Cylinder { center: (1644.033, -788.04, -2406.065), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Kakariko Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/93/TotK_Abandoned_Kakariko_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (1834.819, -627.515, 1198.487), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (1834.819, -627.515, 1198.487), radius: 106.132 },
        Shape::Cylinder { center: (1834.819, -627.515, 1198.487), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Lurelin Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Abandoned_Lurelin_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (2902.169, -436.025, 3381.745), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (2902.169, -436.025, 3381.745), radius: 88.2 },
        Shape::Cylinder { center: (2902.169, -436.025, 3381.745), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Tarrey Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/d/d4/TotK_Abandoned_Tarrey_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (3968.37, -542.007, -1610.529), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (3968.37, -542.007, -1610.529), radius: 72.491 },
        Shape::Cylinder { center: (3968.37, -542.007, -1610.529), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Lanayru Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/0b/TotK_Abandoned_Lanayru_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (3281.13, -723.967, -563.565), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (3281.13, -723.967, -563.565), radius: 74.827 },
        Shape::Cylinder { center: (3281.13, -723.967, -563.565), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },
    Location { name: "Abandoned Hateno Mine", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/2b/TotK_Abandoned_Hateno_Mine.png", category: "AbandonedMine", layer: ZeldaLayer::Underground, center: (3567.668, -601.87, 2238.528), bounding_radius: ABANDONED_MINE_RADIUS, shapes: &[
        Shape::Sphere { center: (3567.668, -601.87, 2238.528), radius: 89.029 },
        Shape::Cylinder { center: (3567.668, -601.87, 2238.528), radius: ABANDONED_MINE_RADIUS, half_height: ABANDONED_MINE_HALF_HEIGHT },
    ] },

    // Sky Archipelagos
    Location { name: "Tabantha Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/80/TotK_Tabantha_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (-3676.139, 1339.59, -664.48), bounding_radius: 707.0, shapes: &[Shape::Box { center: (-3676.139, 1339.59, -664.48), half: (500.0, 400.0, 500.0), rotation_y: 0.0 }] },
    Location { name: "North Gerudo Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_North_Gerudo_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (-3580.118, 1567.0, 526.512), bounding_radius: 700.0, shapes: &[Shape::Cylinder { center: (-3580.118, 1567.0, 526.512), radius: 700.0, half_height: 700.0 }] },
    Location { name: "West Hyrule Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/c/c8/TotK_West_Hyrule_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (-2290.425, 850.0, 411.736), bounding_radius: 212.0, shapes: &[Shape::Box { center: (-2290.425, 850.0, 411.736), half: (150.0, 150.0, 150.0), rotation_y: 0.0 }] },
    Location { name: "South Hyrule Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f9/TotK_South_Hyrule_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (-992.385, 812.406, 1445.262), bounding_radius: 750.0, shapes: &[Shape::Box { center: (-992.385, 812.406, 1445.262), half: (600.0, 400.0, 450.0), rotation_y: 0.872665 }] },
    Location { name: "Faron Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/71/TotK_Faron_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (-112.51, 625.387, 2550.025), bounding_radius: 492.0, shapes: &[Shape::Box { center: (-112.51, 625.387, 2550.025), half: (450.0, 300.0, 200.0), rotation_y: 0.199866 }] },
    Location { name: "Central Hyrule Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/97/TotK_Central_Hyrule_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (246.048, 332.799, -69.875), bounding_radius: 1662.0, shapes: &[Shape::Box { center: (246.048, 332.799, -69.875), half: (1600.0, 150.0, 450.0), rotation_y: 0.0 }] },
    Location { name: "Lanayru Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Lanayru_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (3043.103, 859.863, -114.663), bounding_radius: 646.0, shapes: &[Shape::Box { center: (3043.103, 859.863, -114.663), half: (240.0, 200.0, 600.0), rotation_y: 0.0 }] },
    Location { name: "Sokkala Sky Archipelago", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/2a/TotK_Sokkala_Sky_Archipelago.png", category: "SkyArchipelago", layer: ZeldaLayer::Sky, center: (3755.639, 1022.561, -1675.351), bounding_radius: 500.0, shapes: &[Shape::Box { center: (3755.639, 1022.561, -1675.351), half: (300.0, 300.0, 400.003), rotation_y: 0.334117 }] },
];

const BOTW_LOCATIONS: &[Location] = &[
    // Settlements & Villages
    Location { name: "Rito Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/26/BotW_Rito_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3620.765, 288.672, -1799.816), bounding_radius: 80.0, shapes: &[Shape::Sphere { center: (-3620.765, 288.672, -1799.816), radius: 80.0 }] },
    Location { name: "Gerudo Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/03/BotW_Gerudo_Town.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3844.5, 194.5, 2925.0), bounding_radius: 134.537, shapes: &[Shape::Box { center: (-3844.5, 194.5, 2925.0), half: (100.0, 50.0, 90.0), rotation_y: 0.785398 }] },
    Location { name: "Kara Kara Bazaar", image: "https://cdn.wikimg.net/en/zeldawiki/images/c/cb/BotW_Kara_Kara_Bazaar.png", category: "Village", layer: ZeldaLayer::Ground, center: (-3239.28, 140.356, 2569.85), bounding_radius: 67.501, shapes: &[Shape::Sphere { center: (-3239.28, 140.356, 2569.85), radius: 67.501 }] },
    Location { name: "Korok Forest", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/BotW_Korok_Forest.png", category: "Village", layer: ZeldaLayer::Ground, center: (433.816, 268.455, -2208.076), bounding_radius: 150.0, shapes: &[Shape::Sphere { center: (433.816, 268.455, -2208.076), radius: 150.0 }] },
    Location { name: "Goron City", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/5c/BotW_Goron_City.png", category: "Village", layer: ZeldaLayer::Ground, center: (1680.0, 495.0, -2445.0), bounding_radius: 100.0, shapes: &[Shape::Cylinder { center: (1680.0, 495.0, -2445.0), radius: 100.0, half_height: 45.0 }] },
    Location { name: "Kakariko Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/BotW_Kakariko_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (1852.767, 227.803, 987.262), bounding_radius: 110.0, shapes: &[Shape::Sphere { center: (1852.767, 227.803, 987.262), radius: 110.0 }] },
    Location { name: "Lurelin Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/BotW_Lurelin_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (2868.801, 100.104, 3450.498), bounding_radius: 175.0, shapes: &[Shape::Sphere { center: (2868.801, 100.104, 3450.498), radius: 175.0 }] },
    Location { name: "Tarrey Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/99/BotW_Tarrey_Town.png", category: "Village", layer: ZeldaLayer::Ground, center: (3964.299, 235.622, -1612.209), bounding_radius: 65.0, shapes: &[Shape::Sphere { center: (3964.299, 235.622, -1612.209), radius: 65.0 }] },
    Location { name: "Zora's Domain", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f5/BotW_Zora%27s_Domain.png", category: "Village", layer: ZeldaLayer::Ground, center: (3321.546, 241.756, -502.415), bounding_radius: 150.0, shapes: &[Shape::Sphere { center: (3321.546, 241.756, -502.415), radius: 150.0 }] },
    Location { name: "Hateno Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/10/BotW_Hateno_Village.png", category: "Village", layer: ZeldaLayer::Ground, center: (3515.0, 230.0, 2145.0), bounding_radius: 320.0, shapes: &[
        Shape::Sphere { center: (3683.176, 239.097, 2079.311), radius: 139.152 },
        Shape::Sphere { center: (3379.61, 225.453, 2176.045), radius: 130.0 },
        Shape::Sphere { center: (3337.337, 215.453, 2233.17), radius: 120.0 },
        Shape::Sphere { center: (3598.36, 252.516, 2137.678), radius: 80.0 },
        Shape::Sphere { center: (3478.329, 199.508, 2143.116), radius: 100.0 },
    ] },

    // Stables
    Location { name: "Foothill Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/58/BotW_Foothill_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (2613.332, 253.358, -1143.513), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (2613.332, 253.358, -1143.513), radius: 40.0 }] },
    Location { name: "Dueling Peaks Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/BotW_Dueling_Peaks_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1761.314, 115.573, 1926.244), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (1761.314, 115.573, 1926.244), radius: 40.0 }] },
    Location { name: "Lakeside Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a3/BotW_Lakeside_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1552.023, 166.02, 3537.839), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (1552.023, 166.02, 3537.839), radius: 40.0 }] },
    Location { name: "Highland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/be/BotW_Highland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (529.6, 152.925, 3450.883), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (529.6, 152.925, 3450.883), radius: 40.0 }] },
    Location { name: "Woodland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/BotW_Woodland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (1065.67, 127.565, -1141.583), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (1065.67, 127.565, -1141.583), radius: 40.0 }] },
    Location { name: "Gerudo Canyon Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f2/BotW_Gerudo_Canyon_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-2803.812, 134.951, 2225.765), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-2803.812, 134.951, 2225.765), radius: 40.0 }] },
    Location { name: "Outskirt Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/bc/BotW_Outskirt_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1449.493, 137.751, 1269.011), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-1449.493, 137.751, 1269.011), radius: 40.0 }] },
    Location { name: "Serenne Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/BotW_Serenne_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1559.375, 212.325, -1799.053), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-1559.375, 212.325, -1799.053), radius: 40.0 }] },
    Location { name: "Wetland Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/11/BotW_Wetland_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (888.062, 131.289, 173.672), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (888.062, 131.289, 173.672), radius: 40.0 }] },
    Location { name: "Rito Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/87/BotW_Rito_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-3255.571, 224.466, -1757.625), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-3255.571, 224.466, -1757.625), radius: 40.0 }] },
    Location { name: "Riverside Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/6/65/BotW_Riverside_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (339.232, 115.312, 1095.297), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (339.232, 115.312, 1095.297), radius: 40.0 }] },
    Location { name: "Tabantha Bridge Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/15/BotW_Tabantha_Bridge_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-2931.568, 274.784, -547.598), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-2931.568, 274.784, -547.598), radius: 40.0 }] },
    Location { name: "Snowfield Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/5a/BotW_Snowfield_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (-1654.883, 339.288, -2572.156), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (-1654.883, 339.288, -2572.156), radius: 40.0 }] },
    Location { name: "East Akkala Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/fd/BotW_East_Akkala_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (4227.582, 231.441, -2749.128), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (4227.582, 231.441, -2749.128), radius: 40.0 }] },
    Location { name: "South Akkala Stable", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/95/BotW_South_Akkala_Stable.png", category: "Stable", layer: ZeldaLayer::Ground, center: (3149.832, 306.78, -1692.647), bounding_radius: 40.0, shapes: &[Shape::Sphere { center: (3149.832, 306.78, -1692.647), radius: 40.0 }] },

    // Sheikah Towers
    Location { name: "Hebra Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/ac/BotW_Hebra_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-2173.0, 489.537, -2034.0), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-2173.0, 489.537, -2034.0), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Tabantha Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/8e/BotW_Tabantha_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-3613.748, 405.392, -990.165), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-3613.748, 405.392, -990.165), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Gerudo Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/71/BotW_Gerudo_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-3666.0, 430.656, 1828.6), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-3666.0, 430.656, 1828.6), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Wasteland Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/8c/BotW_Wasteland_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-2306.836, 490.537, 2437.32), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-2306.836, 490.537, 2437.32), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Woodland Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/7e/BotW_Woodland_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (883.884, 310.571, -1605.71), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (883.884, 310.571, -1605.71), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Central Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/BotW_Central_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-788.645, 157.998, 442.031), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-788.645, 157.998, 442.031), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Great Plateau Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/2/27/BotW_Great_Plateau_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-560.035, 206.777, 1694.863), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-560.035, 206.777, 1694.863), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Dueling Peaks Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/b/b4/BotW_Dueling_Peaks_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (1016.777, 144.362, 1714.082), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (1016.777, 144.362, 1714.082), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Lake Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/7e/BotW_Lake_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-31.816, 240.835, 2961.601), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-31.816, 240.835, 2961.601), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Eldin Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/3/34/BotW_Eldin_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (2174.151, 469.084, -1556.781), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (2174.151, 469.084, -1556.781), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Akkala Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/BotW_Akkala_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (3308.0, 554.137, -1500.1), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (3308.0, 554.137, -1500.1), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Lanayru Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/74/BotW_Lanayru_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (2258.0, 271.272, -109.0), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (2258.0, 271.272, -109.0), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Hateno Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/5/54/BotW_Hateno_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (2735.5, 296.537, 2133.5), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (2735.5, 296.537, 2133.5), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Faron Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/3/3c/BotW_Faron_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (1331.203, 230.287, 3273.723), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (1331.203, 230.287, 3273.723), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
    Location { name: "Ridgeland Tower", image: "https://cdn.wikimg.net/en/zeldawiki/images/d/d9/BotW_Ridgeland_Tower.png", category: "Tower", layer: ZeldaLayer::Ground, center: (-1755.3, 288.937, -774.3), bounding_radius: BOTW_TOWER_RADIUS, shapes: &[Shape::Capsule { center: (-1755.3, 288.937, -774.3), radius: BOTW_TOWER_RADIUS, half_height: BOTW_TOWER_HALF_HEIGHT }] },
];

pub fn resolve_totk_location_3d(position: Vector3, layer: ZeldaLayer) -> LocationResult {
    let effective_layer = if layer == ZeldaLayer::Unknown {
        if position.y >= TOTK_SKY_MIN_Y {
            ZeldaLayer::Sky
        } else if position.y <= TOTK_DEPTHS_MAX_Y {
            ZeldaLayer::Underground
        } else {
            ZeldaLayer::Ground
        }
    } else {
        layer
    };

    if effective_layer == ZeldaLayer::Underground
        && position.y <= -600.0
        && (-650.0..=650.0).contains(&position.x)
        && (-1450.0..=-150.0).contains(&position.z)
    {
        return LocationResult {
            name: "Gloom's Origin".to_owned(),
            image_url: "https://cdn.wikimg.net/en/zeldawiki/images/a/a1/TotK_Gloom%27s_Origin.png".to_owned(),
            category: "abyss".to_owned(),
            layer: ZeldaLayer::Underground,
            matched: true,
        };
    }

    resolve_locations(position, effective_layer, TOTK_LOCATIONS)
}

pub fn resolve_botw_location_3d(position: Vector3) -> LocationResult {
    resolve_locations(position, ZeldaLayer::Ground, BOTW_LOCATIONS)
}

fn resolve_locations(position: Vector3, layer: ZeldaLayer, locations: &[Location]) -> LocationResult {
    for location in locations {
        if layer != ZeldaLayer::Unknown
            && location.layer != ZeldaLayer::Unknown
            && layer != location.layer
        {
            continue;
        }
        let dx = position.x - location.center.0;
        let dz = position.z - location.center.2;
        if location.bounding_radius > 0.0
            && dx * dx + dz * dz > location.bounding_radius * location.bounding_radius
        {
            continue;
        }
        if location.shapes.is_empty() || location.shapes.iter().any(|shape| is_within(position, *shape)) {
            return LocationResult {
                name: location.name.to_owned(),
                image_url: location.image.to_owned(),
                category: location.category.to_owned(),
                layer: location.layer,
                matched: true,
            };
        }
    }
    LocationResult::default()
}

fn is_within(position: Vector3, shape: Shape) -> bool {
    match shape {
        Shape::Sphere { center, radius } => {
            let dx = position.x - center.0;
            let dy = position.y - center.1;
            let dz = position.z - center.2;
            dx * dx + dy * dy + dz * dz <= radius * radius
        }
        Shape::Box { center, half, rotation_y } => {
            let dx = position.x - center.0;
            let dy = position.y - center.1;
            let dz = position.z - center.2;
            let cosine = rotation_y.cos();
            let sine = rotation_y.sin();
            let local_x = cosine * dx - sine * dz;
            let local_z = sine * dx + cosine * dz;
            local_x.abs() <= half.0 && dy.abs() <= half.1 && local_z.abs() <= half.2
        }
        Shape::Cylinder { center, radius, half_height } => {
            let dx = position.x - center.0;
            let dy = position.y - center.1;
            let dz = position.z - center.2;
            dx * dx + dz * dz <= radius * radius && dy.abs() <= half_height
        }
        Shape::Capsule { center, radius, half_height } => {
            let dx = position.x - center.0;
            let dy = position.y - center.1;
            let dz = position.z - center.2;
            let nearest_y = dy.clamp(-half_height, half_height);
            let dist_y = dy - nearest_y;
            dx * dx + dist_y * dist_y + dz * dz <= radius * radius
        }
    }
}

pub fn resolve_poi_artwork(poi_name: &str, game: ZeldaGame) -> String {
    if poi_name.is_empty() || game != ZeldaGame::TearsOfTheKingdom {
        return String::new();
    }

    const MAPPINGS: &[(&str, &str)] = &[
        ("Gloom", "https://cdn.wikimg.net/en/zeldawiki/images/a/a1/TotK_Gloom%27s_Origin.png"),
        ("Demon King", "https://cdn.wikimg.net/en/zeldawiki/images/a/a1/TotK_Gloom%27s_Origin.png"),
        ("Tarrey Town", "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png"),
        ("Lookout Landing", "https://cdn.wikimg.net/en/zeldawiki/images/9/94/TotK_Lookout_Landing.png"),
        ("Hateno Village", "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png"),
        ("Kakariko Village", "https://cdn.wikimg.net/en/zeldawiki/images/a/af/TotK_Kakariko_Village.png"),
        ("Goron City", "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png"),
        ("Rito Village", "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png"),
        ("Zora's Domain", "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png"),
        ("Zoras Domain", "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png"),
        ("Gerudo Town", "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png"),
        ("Kara Kara", "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Kara_Kara_Bazaar.png"),
        ("Lurelin", "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png"),
        ("Korok Forest", "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png"),
        ("Hudson Construction", "https://cdn.wikimg.net/en/zeldawiki/images/3/35/TotK_Hudson_Construction_Site.png"),
        ("YunoboCo", "https://cdn.wikimg.net/en/zeldawiki/images/5/55/TotK_YunoboCo_HQ.png"),
        ("Bedrock Bistro", "https://cdn.wikimg.net/en/zeldawiki/images/1/14/TotK_Bedrock_Bistro.png"),
        ("Lucky Clover", "https://cdn.wikimg.net/en/zeldawiki/images/b/b7/TotK_Lucky_Clover_Gazette.png"),
        ("Flight Range", "https://cdn.wikimg.net/en/zeldawiki/images/3/31/TotK_Flight_Range.png"),
        ("Forgotten Temple", "https://cdn.wikimg.net/en/zeldawiki/images/3/33/TotK_Forgotten_Temple.png"),
        ("Hyrule Castle", "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Hyrule_Castle.png"),
        ("Temple of Time", "https://cdn.wikimg.net/en/zeldawiki/images/e/e4/TotK_Temple_of_Time.png"),
        ("Yiga Clan", "https://cdn.wikimg.net/en/zeldawiki/images/8/8d/TotK_Yiga_Clan_Hideout.png"),
        ("East Akkala Stable", "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/TotK_East_Akkala_Stable.png"),
        ("South Akkala Stable", "https://cdn.wikimg.net/en/zeldawiki/images/b/bd/TotK_South_Akkala_Stable.png"),
        ("Dueling Peaks Stable", "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png"),
        ("Tabantha Bridge Stable", "https://cdn.wikimg.net/en/zeldawiki/images/a/a5/TotK_Tabantha_Bridge_Stable.png"),
        ("Woodland Stable", "https://cdn.wikimg.net/en/zeldawiki/images/9/99/TotK_Woodland_Stable.png"),
        ("Foothill Stable", "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Foothill_Stable.png"),
        ("Snowfield Stable", "https://cdn.wikimg.net/en/zeldawiki/images/9/98/TotK_Snowfield_Stable.png"),
        ("Wetland Stable", "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Wetland_Stable.png"),
        ("Outskirt Stable", "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_Outskirt_Stable.png"),
        ("Riverside Stable", "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Riverside_Stable.png"),
        ("New Serenne Stable", "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_New_Serenne_Stable.png"),
        ("Gerudo Canyon Stable", "https://cdn.wikimg.net/en/zeldawiki/images/5/56/TotK_Gerudo_Canyon_Stable.png"),
        ("Highland Stable", "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png"),
        ("Lakeside Stable", "https://cdn.wikimg.net/en/zeldawiki/images/e/e1/TotK_Lakeside_Stable.png"),
        ("Ulri Mountain", "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/TotK_Ulri_Mountain_Skyview_Tower.png"),
        ("Lindor's Brow", "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png"),
        ("Lindors Brow", "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png"),
        ("Pikida Stonegrove", "https://cdn.wikimg.net/en/zeldawiki/images/4/4f/TotK_Pikida_Stonegrove_Skyview_Tower.png"),
        ("Eldin Canyon", "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Eldin_Canyon_Skyview_Tower.png"),
        ("Sahasra Slope", "https://cdn.wikimg.net/en/zeldawiki/images/8/82/TotK_Sahasra_Slope_Skyview_Tower.png"),
        ("Upland Zorana", "https://cdn.wikimg.net/en/zeldawiki/images/6/66/TotK_Upland_Zorana_Skyview_Tower.png"),
        ("Popla Foothills", "https://cdn.wikimg.net/en/zeldawiki/images/b/b2/TotK_Popla_Foothills_Skyview_Tower.png"),
        ("Rabella Wetlands", "https://cdn.wikimg.net/en/zeldawiki/images/c/c9/TotK_Rabella_Wetlands_Skyview_Tower.png"),
        ("Gerudo Canyon Tower", "https://cdn.wikimg.net/en/zeldawiki/images/9/95/TotK_Gerudo_Canyon_Skyview_Tower.png"),
        ("Gerudo Highlands Tower", "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/TotK_Gerudo_Highlands_Skyview_Tower.png"),
        ("Thyphlo Ruins Tower", "https://cdn.wikimg.net/en/zeldawiki/images/e/e6/TotK_Thyphlo_Ruins_Skyview_Tower.png"),
        ("Mount Lanayru Tower", "https://cdn.wikimg.net/en/zeldawiki/images/7/76/TotK_Mount_Lanayru_Skyview_Tower.png"),
        ("Rospro Pass", "https://cdn.wikimg.net/en/zeldawiki/images/4/45/TotK_Rospro_Pass_Skyview_Tower.png"),
        ("Hyrule Field Tower", "https://cdn.wikimg.net/en/zeldawiki/images/2/2f/TotK_Hyrule_Field_Skyview_Tower.png"),
        ("Great Sky Island", "https://cdn.wikimg.net/en/zeldawiki/images/6/6c/TotK_Great_Sky_Island.png"),
        ("Sokkala", "https://cdn.wikimg.net/en/zeldawiki/images/2/2a/TotK_Sokkala_Sky_Archipelago.png"),
        ("Dragonhead", "https://cdn.wikimg.net/en/zeldawiki/images/f/f0/TotK_Dragonhead_Island.png"),
        ("Starview", "https://cdn.wikimg.net/en/zeldawiki/images/4/42/TotK_Starview_Island.png"),
        ("Wind Temple", "https://cdn.wikimg.net/en/zeldawiki/images/d/db/TotK_Wind_Temple_B2.png"),
        ("Water Temple", "https://cdn.wikimg.net/en/zeldawiki/images/9/96/TotK_Water_Temple_1F.png"),
        ("Fire Temple", "https://cdn.wikimg.net/en/zeldawiki/images/b/bc/TotK_Fire_Temple_2F.png"),
        ("Lightning Temple", "https://cdn.wikimg.net/en/zeldawiki/images/8/81/TotK_Lightning_Temple_7F.png"),
        ("Spirit Temple", "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Spirit_Temple_Interior.png"),
        ("Lanayru Sky", "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Lanayru_Sky_Archipelago.png"),
        ("Tabantha Sky", "https://cdn.wikimg.net/en/zeldawiki/images/8/80/TotK_Tabantha_Sky_Archipelago.png"),
        ("North Gerudo Sky", "https://cdn.wikimg.net/en/zeldawiki/images/4/47/TotK_North_Gerudo_Sky_Archipelago.png"),
        ("South Hyrule Sky", "https://cdn.wikimg.net/en/zeldawiki/images/f/f9/TotK_South_Hyrule_Sky_Archipelago.png"),
        ("Central Hyrule Sky", "https://cdn.wikimg.net/en/zeldawiki/images/9/97/TotK_Central_Hyrule_Sky_Archipelago.png"),
        ("West Hyrule Sky", "https://cdn.wikimg.net/en/zeldawiki/images/c/c8/TotK_West_Hyrule_Sky_Archipelago.png"),
        ("Faron Sky", "https://cdn.wikimg.net/en/zeldawiki/images/7/71/TotK_Faron_Sky_Archipelago.png"),
        ("Central Mine", "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Great_Abandoned_Central_Mine.png"),
        ("Tarrey Mine", "https://cdn.wikimg.net/en/zeldawiki/images/d/d4/TotK_Abandoned_Tarrey_Mine.png"),
        ("Lanayru Mine", "https://cdn.wikimg.net/en/zeldawiki/images/0/0b/TotK_Abandoned_Lanayru_Mine.png"),
        ("Gerudo Mine", "https://cdn.wikimg.net/en/zeldawiki/images/0/0c/TotK_Abandoned_Gerudo_Mine.png"),
        ("Hebra Mine", "https://cdn.wikimg.net/en/zeldawiki/images/2/28/TotK_Abandoned_Hebra_Mine.png"),
        ("Eldin Mine", "https://cdn.wikimg.net/en/zeldawiki/images/7/75/TotK_Abandoned_Eldin_Mine.png"),
        ("Kakariko Mine", "https://cdn.wikimg.net/en/zeldawiki/images/9/93/TotK_Abandoned_Kakariko_Mine.png"),
        ("Hateno Mine", "https://cdn.wikimg.net/en/zeldawiki/images/2/2b/TotK_Abandoned_Hateno_Mine.png"),
        ("Lurelin Mine", "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Abandoned_Lurelin_Mine.png"),
        ("Kara Kara Mine", "https://cdn.wikimg.net/en/zeldawiki/images/5/5d/TotK_Abandoned_Kara_Kara_Mine.png"),
        ("Construct Factory", "https://cdn.wikimg.net/en/zeldawiki/images/e/e9/TotK_Construct_Factory.png"),
    ];

    MAPPINGS
        .iter()
        .find_map(|(needle, url)| poi_name.contains(needle).then_some(*url))
        .unwrap_or_default()
        .to_owned()
}

pub fn resolve_region_artwork(region: &str, game: ZeldaGame, layer: ZeldaLayer) -> String {
    let url = match game {
        ZeldaGame::TearsOfTheKingdom if layer == ZeldaLayer::Sky => "https://cdn.wikimg.net/en/zeldawiki/images/6/6c/TotK_Great_Sky_Island.png",
        ZeldaGame::TearsOfTheKingdom if layer == ZeldaLayer::Underground => "https://cdn.wikimg.net/en/zeldawiki/images/1/17/TotK_Depths_Promotional_Screenshot.jpg",
        ZeldaGame::TearsOfTheKingdom => match region {
            "Akkala" => "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png",
            "Central Hyrule" | "Hyrule Field" => "https://cdn.wikimg.net/en/zeldawiki/images/0/01/TotK_Hyrule_Castle.png",
            "Eldin" => "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png",
            "Gerudo" => "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png",
            "Hebra" => "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png",
            "Lanayru" => "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png",
            "Necluda" => "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png",
            "Faron" => "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png",
            "Great Hyrule Forest" => "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png",
            "Hyrule Ridge" => "https://cdn.wikimg.net/en/zeldawiki/images/7/7c/TotK_Lindor%27s_Brow_Skyview_Tower.png",
            "Dueling Peaks" => "https://cdn.wikimg.net/en/zeldawiki/images/5/53/TotK_Dueling_Peaks_Stable.png",
            "Lake Hylia" => "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Highland_Stable.png",
            _ => "",
        },
        ZeldaGame::BreathOfTheWild => match region {
            "Akkala" => "https://cdn.wikimg.net/en/zeldawiki/images/f/ff/BotW_Akkala_Tower.png",
            "Central Hyrule" => "https://cdn.wikimg.net/en/zeldawiki/images/b/b0/BotW_Central_Tower.png",
            "Eldin" => "https://cdn.wikimg.net/en/zeldawiki/images/5/5c/BotW_Goron_City.png",
            "Gerudo" => "https://cdn.wikimg.net/en/zeldawiki/images/0/03/BotW_Gerudo_Town.png",
            "Hebra" => "https://cdn.wikimg.net/en/zeldawiki/images/2/26/BotW_Rito_Village.png",
            "Lanayru" => "https://cdn.wikimg.net/en/zeldawiki/images/f/f5/BotW_Zora%27s_Domain.png",
            "Necluda" => "https://cdn.wikimg.net/en/zeldawiki/images/1/10/BotW_Hateno_Village.png",
            "Faron" => "https://cdn.wikimg.net/en/zeldawiki/images/f/f3/BotW_Lurelin_Village.png",
            "Great Hyrule Forest" => "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/BotW_Korok_Forest.png",
            _ => "",
        },
        ZeldaGame::Unknown => "",
    };
    url.to_owned()
}

#[cfg(test)]
mod tests {
    use super::{
        Vector3, ZeldaGame, ZeldaLayer, resolve_botw_location_3d, resolve_poi_artwork,
        resolve_region_artwork, resolve_totk_location_3d,
    };

    #[test]
    fn recognizes_totk_ground_depths_and_sky() {
        let lookout = Vector3::new(-254.12, 123.447, -101.602).expect("valid point");
        assert_eq!(resolve_totk_location_3d(lookout, ZeldaLayer::Ground).name(), "Lookout Landing");

        let gloom = Vector3::new(0.0, -700.0, -500.0).expect("valid point");
        assert_eq!(resolve_totk_location_3d(gloom, ZeldaLayer::Unknown).name(), "Gloom's Origin");

        let sky = Vector3::new(-3676.139, 1339.59, -664.48).expect("valid point");
        assert_eq!(resolve_totk_location_3d(sky, ZeldaLayer::Unknown).name(), "Tabantha Sky Archipelago");
    }

    #[test]
    fn recognizes_botw_capsule_tower() {
        let tower = Vector3::new(-2173.0, 489.537, -2034.0).expect("valid point");
        assert_eq!(resolve_botw_location_3d(tower).name(), "Hebra Tower");
    }

    #[test]
    fn keeps_cpp_artwork_mappings() {
        assert!(resolve_poi_artwork("Wind Temple", ZeldaGame::TearsOfTheKingdom).contains("Wind_Temple"));
        assert!(resolve_poi_artwork("Wind Temple", ZeldaGame::BreathOfTheWild).is_empty());
        assert!(resolve_region_artwork("Akkala", ZeldaGame::BreathOfTheWild, ZeldaLayer::Ground).contains("BotW_Akkala_Tower"));
    }
}
