//! Album synchronization and v1-compatible folder matching.

use crate::config::{ConfigManager, default_album_folder};
use crate::coral::CoralClient;
use crate::game_aliases::GAME_ALIAS_GROUPS;
use crate::http::HttpClient;
use crate::model::{MediaItem, SyncResult};
use filetime::{FileTime, set_file_mtime};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use unicode_normalization::{UnicodeNormalization as _, char::is_combining_mark};
use walkdir::WalkDir;

const MAX_MEDIA_DOWNLOAD_BYTES: usize = 256 * 1024 * 1024;

pub struct SyncEngine {
    config: ConfigManager,
    coral: Arc<CoralClient>,
    http: HttpClient,
}

impl SyncEngine {
    pub fn new(config: ConfigManager, coral: Arc<CoralClient>, http: HttpClient) -> Self {
        Self { config, coral, http }
    }

    pub fn sync<F>(&self, cancelled: F) -> anyhow::Result<SyncResult>
    where
        F: Fn() -> bool,
    {
        let config = self.config.snapshot();
        anyhow::ensure!(
            !config.session_token().is_empty(),
            "Not signed in to Nintendo Account"
        );
        check_cancelled(&cancelled)?;
        let media = self.coral.media_list(config.session_token())?;
        check_cancelled(&cancelled)?;

        let root = if config.destination_folder().trim().is_empty() {
            default_album_folder()
        } else {
            PathBuf::from(config.destination_folder())
        };
        fs::create_dir_all(&root)?;
        let mut existing = index_existing_album(&root, &cancelled)?;
        let title_folders = learn_title_folders(&media, &existing);
        let root_is_album = root
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name.eq_ignore_ascii_case("album"));
        let album_directory = if root_is_album {
            root.clone()
        } else {
            root.join("Album")
        };

        let mut downloaded = 0_usize;
        for item in &media {
            check_cancelled(&cancelled)?;
            let timestamp = item.timestamp();
            let prefix = capture_timestamp_prefix(timestamp);
            let extension = if item.media_type().eq_ignore_ascii_case("video") {
                "mp4"
            } else {
                "jpg"
            };
            let filename = format!("{prefix}_c.{extension}");
            if existing.names.contains(&filename.to_ascii_lowercase())
                || existing.names.contains(&prefix.to_ascii_lowercase())
            {
                continue;
            }
            validate_media_item(item)?;

            let game_folder = title_folders
                .get(&item.title_id().to_ascii_lowercase())
                .cloned()
                .unwrap_or_else(|| resolve_game_folder(&album_directory, item.app_name()));
            let destination = album_directory.join(game_folder).join(&filename);
            if let Some(parent) = destination.parent() {
                fs::create_dir_all(parent)?;
            }

            // Redirects are disabled for media. Validation therefore applies to
            // the actual host that receives the request, not just the first URL.
            let response = self.http.get_no_redirect(
                item.content_uri(),
                &[],
                60,
                MAX_MEDIA_DOWNLOAD_BYTES,
            )?;
            anyhow::ensure!(
                response.status() / 100 == 2,
                "Media download failed (HTTP {})",
                response.status()
            );
            let expected_length = usize::try_from(item.content_length()).unwrap_or(usize::MAX);
            anyhow::ensure!(
                response.body().len() == expected_length,
                "Media download size did not match Nintendo's content length"
            );
            anyhow::ensure!(
                response.body().len() <= MAX_MEDIA_DOWNLOAD_BYTES,
                "Media download exceeded the 256 MiB safety limit"
            );
            check_cancelled(&cancelled)?;

            let part = with_appended_suffix(&destination, ".part");
            let _ = fs::remove_file(&part);
            let write_result = (|| -> anyhow::Result<()> {
                let mut file = File::create(&part)?;
                file.write_all(response.body())?;
                file.sync_all()?;
                check_cancelled(&cancelled)?;
                fs::rename(&part, &destination)?;
                Ok(())
            })();
            if let Err(error) = write_result {
                let _ = fs::remove_file(&part);
                return Err(error);
            }

            preserve_capture_timestamp(&destination, timestamp);
            existing.names.insert(filename.to_ascii_lowercase());
            existing.names.insert(prefix.to_ascii_lowercase());
            downloaded += 1;
        }

        Ok(SyncResult::new(media.len(), downloaded))
    }
}

fn validate_media_item(item: &MediaItem) -> anyhow::Result<()> {
    anyhow::ensure!(
        item.content_length() > 0,
        "Nintendo media item has no valid content length"
    );
    anyhow::ensure!(
        item.content_length() <= MAX_MEDIA_DOWNLOAD_BYTES as i64,
        "Nintendo media item exceeds the 256 MiB safety limit"
    );
    let url = url::Url::parse(item.content_uri())?;
    anyhow::ensure!(url.scheme() == "https", "Nintendo media URL must use HTTPS");
    anyhow::ensure!(
        url.username().is_empty() && url.password().is_none(),
        "Nintendo media URL cannot contain credentials"
    );
    anyhow::ensure!(
        url.port_or_known_default() == Some(443),
        "Nintendo media URL must use the standard HTTPS port"
    );
    let host = url
        .host_str()
        .ok_or_else(|| anyhow::anyhow!("Nintendo media URL is missing a hostname"))?;
    anyhow::ensure!(host.contains('.'), "Nintendo media hostname is not public-looking");
    anyhow::ensure!(
        host.parse::<std::net::IpAddr>().is_err(),
        "Nintendo media URL cannot use a numeric IP address"
    );
    anyhow::ensure!(
        host.bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-')),
        "Nintendo media hostname contains invalid characters"
    );
    let lower = host.to_ascii_lowercase();
    let denied_suffixes = [
        "localhost",
        ".localhost",
        ".local",
        ".localdomain",
        ".internal",
        ".lan",
        ".home",
    ];
    anyhow::ensure!(
        !denied_suffixes
            .iter()
            .any(|suffix| lower == suffix.trim_start_matches('.') || lower.ends_with(suffix)),
        "Nintendo media URL resolves to a local hostname namespace"
    );
    Ok(())
}

#[derive(Default)]
struct ExistingAlbumIndex {
    names: HashSet<String>,
    folder_by_timestamp_prefix: HashMap<String, String>,
}

fn index_existing_album<F>(root: &Path, cancelled: &F) -> anyhow::Result<ExistingAlbumIndex>
where
    F: Fn() -> bool,
{
    let mut index = ExistingAlbumIndex::default();
    for entry in WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_map(Result::ok)
    {
        check_cancelled(cancelled)?;
        if !entry.file_type().is_file() {
            continue;
        }
        let filename = entry.file_name().to_string_lossy().into_owned();
        let lower = filename.to_ascii_lowercase();
        if lower.ends_with(".part") || lower.ends_with(".tmp") {
            continue;
        }
        if entry
            .metadata()
            .map(|metadata| metadata.len() == 0)
            .unwrap_or(true)
        {
            continue;
        }
        let prefix = prefix_from_existing_filename(entry.path());
        index.names.insert(lower);
        index.names.insert(prefix.to_ascii_lowercase());
        if let Some(parent) = entry
            .path()
            .parent()
            .and_then(Path::file_name)
            .and_then(|name| name.to_str())
        {
            index
                .folder_by_timestamp_prefix
                .entry(prefix.to_ascii_lowercase())
                .or_insert_with(|| parent.to_owned());
        }
    }
    Ok(index)
}

fn learn_title_folders(
    media: &[MediaItem],
    existing: &ExistingAlbumIndex,
) -> HashMap<String, String> {
    let mut folders = HashMap::new();
    for item in media {
        if item.title_id().is_empty() {
            continue;
        }
        let prefix = capture_timestamp_prefix(item.timestamp()).to_ascii_lowercase();
        if let Some(folder) = existing.folder_by_timestamp_prefix.get(&prefix) {
            folders.insert(item.title_id().to_ascii_lowercase(), folder.clone());
        }
    }
    folders
}

fn resolve_game_folder(album_directory: &Path, app_name: &str) -> String {
    let default_clean = sanitize_folder_v1(app_name);
    if app_name.trim().is_empty() || !album_directory.is_dir() {
        return default_clean;
    }

    let directories = fs::read_dir(album_directory)
        .ok()
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .filter(|entry| entry.file_type().map(|kind| kind.is_dir()).unwrap_or(false))
        .filter_map(|entry| entry.file_name().into_string().ok())
        .map(|name| {
            let normalized = normalize_for_matching_v1(&name);
            (name, normalized)
        })
        .collect::<Vec<_>>();

    let normalized_app = normalize_for_matching_v1(app_name);
    let synonyms: Vec<&str> = GAME_ALIAS_GROUPS
        .iter()
        .find(|group| {
            group
                .iter()
                .any(|alias| normalize_for_matching_v1(alias) == normalized_app)
        })
        .map(|group| group.to_vec())
        .unwrap_or_else(|| vec![app_name]);

    for synonym in synonyms {
        let clean = sanitize_folder_v1(synonym);
        let normalized = normalize_for_matching_v1(synonym);
        if let Some((name, _)) = directories
            .iter()
            .find(|(name, _)| equal_v1_name(name, &clean))
        {
            return name.clone();
        }
        if let Some((name, _)) = directories
            .iter()
            .find(|(_, candidate)| candidate == &normalized)
        {
            return name.clone();
        }
    }

    if normalized_app.len() >= 6 {
        if let Some((name, _)) = directories.iter().find(|(_, candidate)| {
            candidate.len() >= 6
                && (candidate.contains(&normalized_app) || normalized_app.contains(candidate))
        }) {
            return name.clone();
        }
    }

    default_clean
}

fn sanitize_folder_v1(value: &str) -> String {
    if value.trim().is_empty() {
        return "Other".to_owned();
    }
    let mut clean = String::with_capacity(value.len());
    for character in value.chars() {
        if character.is_control() {
            continue;
        }
        if is_unicode_dash_or_hyphen(character) {
            clean.push('-');
            continue;
        }
        if is_invalid_filename_character(character) {
            continue;
        }
        clean.push(character);
    }

    let mut collapsed = String::with_capacity(clean.len());
    let mut previous_space = false;
    for character in clean.chars() {
        if character == ' ' {
            if !previous_space && !collapsed.is_empty() {
                collapsed.push(' ');
                previous_space = true;
            }
        } else {
            collapsed.push(character);
            previous_space = false;
        }
    }
    while matches!(collapsed.chars().last(), Some(' ' | '.')) {
        collapsed.pop();
    }
    let result = collapsed.trim().to_owned();
    if result.is_empty() { "Other".to_owned() } else { result }
}

fn is_invalid_filename_character(character: char) -> bool {
    matches!(
        character,
        '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*'
            | '\u{FF1C}' | '\u{FF1E}' | '\u{FF1A}' | '\u{FF02}' | '\u{FF0F}'
            | '\u{FF3C}' | '\u{FF5C}' | '\u{FF1F}' | '\u{FF0A}'
    )
}

fn is_unicode_dash_or_hyphen(character: char) -> bool {
    matches!(
        character,
        '\u{2010}' | '\u{2011}' | '\u{2012}' | '\u{2013}' | '\u{2014}'
            | '\u{2015}' | '\u{2212}' | '\u{FE58}' | '\u{FE63}' | '\u{FF0D}'
    )
}

fn normalize_for_matching_v1(value: &str) -> String {
    let mut output = String::with_capacity(value.len());
    for character in value.nfd() {
        if is_combining_mark(character) {
            continue;
        }
        if character.is_ascii() {
            if character.is_ascii_alphanumeric() {
                output.push(character.to_ascii_lowercase());
            }
        } else {
            output.push(character);
        }
    }
    output
}

fn equal_v1_name(left: &str, right: &str) -> bool {
    left.eq_ignore_ascii_case(right)
}

fn capture_timestamp_prefix(timestamp: i64) -> String {
    use chrono::{Local, TimeZone as _};
    let supplied = if timestamp > 10_000_000_000 {
        timestamp / 1000
    } else {
        timestamp
    };
    let seconds = if supplied <= 0 {
        Local::now().timestamp()
    } else {
        supplied
    };
    let local = Local
        .timestamp_opt(seconds, 0)
        .single()
        .or_else(|| Local.timestamp_opt(seconds, 0).earliest())
        .unwrap_or_else(Local::now);
    local.format("%Y%m%d%H%M%S00").to_string()
}

fn prefix_from_existing_filename(path: &Path) -> String {
    let mut prefix = path
        .file_stem()
        .and_then(|name| name.to_str())
        .unwrap_or_default()
        .to_owned();
    if prefix.ends_with("_c") {
        prefix.truncate(prefix.len().saturating_sub(2));
    } else if prefix.ends_with("-00") {
        prefix.truncate(prefix.len().saturating_sub(3));
    }
    prefix
}

fn preserve_capture_timestamp(path: &Path, timestamp: i64) {
    if timestamp <= 0 {
        return;
    }
    let seconds = if timestamp > 10_000_000_000 {
        timestamp / 1000
    } else {
        timestamp
    };
    let _ = set_file_mtime(path, FileTime::from_unix_time(seconds, 0));
}

fn with_appended_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(suffix);
    PathBuf::from(value)
}

fn check_cancelled<F>(cancelled: &F) -> anyhow::Result<()>
where
    F: Fn() -> bool,
{
    anyhow::ensure!(!cancelled(), "sync cancelled");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{normalize_for_matching_v1, sanitize_folder_v1, validate_media_item};
    use crate::model::MediaItem;

    #[test]
    fn folder_matching_retains_v1_semantics() {
        assert_eq!(normalize_for_matching_v1("Pokémon: Violet"), "pokemonviolet");
        assert_eq!(sanitize_folder_v1("Bad:/Name*"), "BadName");
        assert_eq!(sanitize_folder_v1("A—B"), "A-B");
    }

    #[test]
    fn media_validation_rejects_local_hostnames() {
        let item: MediaItem = serde_json::from_value(serde_json::json!({
            "contentUri": "https://localhost/x",
            "contentLength": 10
        }))
        .expect("deserialize media");
        assert!(validate_media_item(&item).is_err());
    }
}
