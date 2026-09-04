//! Pure Zelda Notes geometry and region artwork helpers.

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum ZeldaGame { #[default] Unknown, BreathOfTheWild, TearsOfTheKingdom }

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum ZeldaLayer { #[default] Unknown, Ground, Sky, Underground }

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Vector3 { x: f64, y: f64, z: f64 }
impl Vector3 {
    pub fn new(x: f64, y: f64, z: f64) -> Option<Self> { (x.is_finite() && y.is_finite() && z.is_finite()).then_some(Self { x, y, z }) }
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
}

#[derive(Clone, Copy)]
struct Location {
    name: &'static str,
    image: &'static str,
    category: &'static str,
    layer: ZeldaLayer,
    broad_center: (f64, f64),
    broad_radius: f64,
    shapes: &'static [Shape],
}

const TOTK_LOCATIONS: &[Location] = &[
    Location { name: "Lookout Landing", image: "https://cdn.wikimg.net/en/zeldawiki/images/9/94/TotK_Lookout_Landing.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (-254.12, -101.602), broad_radius: 83.45, shapes: &[Shape::Box { center: (-254.12, 123.447, -101.602), half: (55.012, 16.807, 62.732), rotation_y: 0.0 }] },
    Location { name: "Tarrey Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/7/77/TotK_Tarrey_Town.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (3964.299, -1612.209), broad_radius: 85.0, shapes: &[Shape::Sphere { center: (3964.299, 152.472, -1612.209), radius: 85.0 }] },
    Location { name: "Rito Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/6/68/TotK_Rito_Village.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (-3501.0, -1835.0), broad_radius: 210.0, shapes: &[Shape::Sphere { center: (-3620.765, 208.672, -1799.816), radius: 80.0 }, Shape::Sphere { center: (-3482.0, 208.672, -1841.5), radius: 50.0 }, Shape::Sphere { center: (-3429.5, 215.5, -1886.5), radius: 40.0 }, Shape::Sphere { center: (-3522.5, 230.0, -1825.0), radius: 40.0 }] },
    Location { name: "Gerudo Town", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/ad/TotK_Gerudo_Town.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (-3844.5, 2926.197), broad_radius: 134.537, shapes: &[Shape::Box { center: (-3844.5, 149.169, 2926.197), half: (100.0, 50.0, 90.0), rotation_y: 0.785398 }] },
    Location { name: "Kara Kara Bazaar", image: "https://cdn.wikimg.net/en/zeldawiki/images/8/86/TotK_Kara_Kara_Bazaar.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (-3239.28, 2569.85), broad_radius: 67.501, shapes: &[Shape::Sphere { center: (-3239.28, 72.855, 2569.85), radius: 67.501 }] },
    Location { name: "Korok Forest", image: "https://cdn.wikimg.net/en/zeldawiki/images/0/00/TotK_Korok_Forest.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (433.816, -2208.076), broad_radius: 150.0, shapes: &[Shape::Sphere { center: (433.816, 118.455, -2208.076), radius: 150.0 }] },
    Location { name: "Goron City", image: "https://cdn.wikimg.net/en/zeldawiki/images/3/38/TotK_Goron_City.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (1680.0, -2445.0), broad_radius: 170.0, shapes: &[Shape::Cylinder { center: (1680.0, 450.0, -2445.0), radius: 100.0, half_height: 45.0 }, Shape::Cylinder { center: (1734.077, 469.109, -2547.36), radius: 55.168, half_height: 55.168 }] },
    Location { name: "Kakariko Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/af/TotK_Kakariko_Village.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (1852.767, 987.262), broad_radius: 220.0, shapes: &[Shape::Sphere { center: (1852.767, 117.803, 987.262), radius: 110.0 }, Shape::Cylinder { center: (1710.002, 215.753, 930.023), radius: 70.0, half_height: 101.232 }] },
    Location { name: "Lurelin Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/1/1d/TotK_Lurelin_Village.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (2990.0, 3620.0), broad_radius: 400.0, shapes: &[Shape::Sphere { center: (2868.801, -74.896, 3450.498), radius: 175.0 }, Shape::Sphere { center: (3035.962, -99.205, 3656.41), radius: 150.0 }, Shape::Sphere { center: (3037.204, -99.205, 3787.015), radius: 150.0 }] },
    Location { name: "Zora's Domain", image: "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Zora%27s_Domain.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (3319.414, -503.831), broad_radius: 164.0, shapes: &[Shape::Cylinder { center: (3319.414, 215.431, -503.831), radius: 164.0, half_height: 90.0 }] },
    Location { name: "Hateno Village", image: "https://cdn.wikimg.net/en/zeldawiki/images/f/f1/TotK_Hateno_Village.png", category: "Village", layer: ZeldaLayer::Ground, broad_center: (3470.0, 2110.0), broad_radius: 320.0, shapes: &[Shape::Sphere { center: (3379.61, 44.254, 2161.608), radius: 140.0 }, Shape::Sphere { center: (3478.329, 99.508, 2143.116), radius: 100.0 }, Shape::Sphere { center: (3547.945, 166.363, 2010.239), radius: 115.0 }] },
];

pub fn resolve_totk_location_3d(position: Vector3, layer: ZeldaLayer) -> LocationResult {
    resolve_locations(position, layer, TOTK_LOCATIONS)
}

pub fn resolve_botw_location_3d(position: Vector3) -> LocationResult {
    // Shared settlements retain nearly identical world coordinates between the two games.
    // Dynamic Complete Guide data remains the primary source for all other BotW POIs.
    resolve_locations(position, ZeldaLayer::Ground, TOTK_LOCATIONS)
}

fn resolve_locations(position: Vector3, layer: ZeldaLayer, locations: &[Location]) -> LocationResult {
    for location in locations {
        if layer != ZeldaLayer::Unknown && location.layer != ZeldaLayer::Unknown && layer != location.layer { continue; }
        let dx = position.x - location.broad_center.0;
        let dz = position.z - location.broad_center.1;
        if dx * dx + dz * dz > location.broad_radius * location.broad_radius { continue; }
        if location.shapes.iter().any(|shape| is_within(position, *shape)) {
            return LocationResult { name: location.name.to_owned(), image_url: location.image.to_owned(), category: location.category.to_owned(), layer: location.layer, matched: true };
        }
    }
    LocationResult::default()
}

fn is_within(position: Vector3, shape: Shape) -> bool {
    match shape {
        Shape::Sphere { center, radius } => {
            let dx = position.x - center.0; let dy = position.y - center.1; let dz = position.z - center.2;
            dx * dx + dy * dy + dz * dz <= radius * radius
        }
        Shape::Cylinder { center, radius, half_height } => {
            let dx = position.x - center.0; let dy = position.y - center.1; let dz = position.z - center.2;
            dx * dx + dz * dz <= radius * radius && dy.abs() <= half_height
        }
        Shape::Box { center, half, rotation_y } => {
            let dx = position.x - center.0; let dy = position.y - center.1; let dz = position.z - center.2;
            let cosine = rotation_y.cos(); let sine = rotation_y.sin();
            let local_x = cosine * dx - sine * dz; let local_z = sine * dx + cosine * dz;
            local_x.abs() <= half.0 && dy.abs() <= half.1 && local_z.abs() <= half.2
        }
    }
}

pub fn resolve_poi_artwork(name: &str, game: ZeldaGame) -> String {
    let lower = name.to_ascii_lowercase();
    if let Some(location) = TOTK_LOCATIONS.iter().find(|location| location.name.to_ascii_lowercase() == lower) { return location.image.to_owned(); }
    if lower.contains("hyrule castle") { return "https://cdn.wikimg.net/en/zeldawiki/images/5/5d/TotK_Hyrule_Castle.png".to_owned(); }
    if lower.contains("temple of time") { return "https://cdn.wikimg.net/en/zeldawiki/images/3/34/TotK_Temple_of_Time.png".to_owned(); }
    if lower.contains("stable") { return match game { ZeldaGame::BreathOfTheWild => "https://cdn.wikimg.net/en/zeldawiki/images/3/34/BotW_Dueling_Peaks_Stable.png", _ => "https://cdn.wikimg.net/en/zeldawiki/images/0/0b/TotK_New_Serenne_Stable.png" }.to_owned(); }
    String::new()
}

pub fn resolve_region_artwork(region: &str, game: ZeldaGame, layer: ZeldaLayer) -> String {
    if game == ZeldaGame::TearsOfTheKingdom && layer == ZeldaLayer::Sky { return "https://cdn.wikimg.net/en/zeldawiki/images/1/14/TotK_Great_Sky_Island.png".to_owned(); }
    if game == ZeldaGame::TearsOfTheKingdom && layer == ZeldaLayer::Underground { return "https://cdn.wikimg.net/en/zeldawiki/images/7/75/TotK_The_Depths.png".to_owned(); }
    match region {
        "Hebra" | "Tabantha" => "https://cdn.wikimg.net/en/zeldawiki/images/8/80/TotK_Hebra_Mountains.png",
        "Gerudo" => "https://cdn.wikimg.net/en/zeldawiki/images/1/10/TotK_Gerudo_Desert.png",
        "Eldin" | "Death Mountain" => "https://cdn.wikimg.net/en/zeldawiki/images/2/29/TotK_Death_Mountain.png",
        "Akkala" => "https://cdn.wikimg.net/en/zeldawiki/images/a/a2/TotK_Akkala_Highlands.png",
        "Lanayru" => "https://cdn.wikimg.net/en/zeldawiki/images/7/7f/TotK_Lanayru_Great_Spring.png",
        "Necluda" | "Dueling Peaks" => "https://cdn.wikimg.net/en/zeldawiki/images/8/83/TotK_West_Necluda.png",
        "Faron" | "Lake Hylia" => "https://cdn.wikimg.net/en/zeldawiki/images/2/25/TotK_Faron_Grasslands.png",
        "Great Hyrule Forest" => "https://cdn.wikimg.net/en/zeldawiki/images/7/7d/TotK_Great_Hyrule_Forest.png",
        _ => "https://cdn.wikimg.net/en/zeldawiki/images/4/4e/TotK_Hyrule_Field.png",
    }.to_owned()
}

#[cfg(test)]
mod tests {
    use super::{Vector3, ZeldaLayer, resolve_totk_location_3d};

    #[test]
    fn recognizes_lookout_landing() {
        let position = Vector3::new(-254.12, 123.447, -101.602).expect("valid point");
        assert!(resolve_totk_location_3d(position, ZeldaLayer::Ground).matched());
    }
}
