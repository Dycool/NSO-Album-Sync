#include "nso_album_sync/sync.hpp"

#include "nso_album_sync/game_aliases.hpp"
#include "nso_album_sync/util.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#endif

#include <chrono>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace nso {
namespace {

struct ExistingAlbumIndex {
    std::set<std::string> filenames_and_prefixes;
    std::unordered_map<std::string, std::string> folder_by_timestamp_prefix;
};

bool should_cancel(const std::function<bool()>& cancelled) {
    return cancelled && cancelled();
}

void throw_if_cancelled(const std::function<bool()>& cancelled) {
    if (should_cancel(cancelled)) throw std::runtime_error("Sync cancelled");
}

std::string capture_timestamp_prefix(std::int64_t timestamp) {
    if (timestamp <= 0) timestamp = unix_now();
    const auto seconds = timestamp > 10'000'000'000LL ? timestamp / 1000 : timestamp;
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &local_time);
    return std::string(buffer) + "00";
}

std::string prefix_from_existing_filename(const std::filesystem::path& path) {
    std::string prefix = path.stem().string();
    if (prefix.size() > 2 && prefix.ends_with("_c")) {
        prefix.resize(prefix.size() - 2);
    } else if (prefix.size() > 3 && prefix.ends_with("-00")) {
        prefix.resize(prefix.size() - 3);
    }
    return prefix;
}

ExistingAlbumIndex index_existing_album(
    const std::filesystem::path& root,
    const std::function<bool()>& cancelled) {
    ExistingAlbumIndex index;
    for (auto iterator = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied);
         iterator != std::filesystem::recursive_directory_iterator();
         ++iterator) {
        throw_if_cancelled(cancelled);
        if (!iterator->is_regular_file()) continue;

        const auto filename_lower = lower(iterator->path().filename().string());
        if (filename_lower.ends_with(".part") || filename_lower.ends_with(".tmp")) {
            continue;
        }

        std::error_code error;
        if (iterator->file_size(error) == 0) continue;
        const auto& path = iterator->path();
        const auto filename = path.filename().string();
        const auto prefix = prefix_from_existing_filename(path);
        index.filenames_and_prefixes.insert(lower(filename));
        index.filenames_and_prefixes.insert(lower(prefix));
        index.folder_by_timestamp_prefix.emplace(
            lower(prefix), path.parent_path().filename().string());
    }
    return index;
}

std::unordered_map<std::string, std::string> learn_title_folders(
    const std::vector<MediaItem>& media,
    const ExistingAlbumIndex& existing) {
    std::unordered_map<std::string, std::string> folders;
    for (const auto& item : media) {
        if (item.title_id.empty()) continue;
        const auto timestamp = item.captured_at != 0 ? item.captured_at : item.uploaded_at;
        const auto prefix = capture_timestamp_prefix(timestamp);
        const auto existing_folder =
            existing.folder_by_timestamp_prefix.find(lower(prefix));
        if (existing_folder != existing.folder_by_timestamp_prefix.end()) {
            folders[item.title_id] = existing_folder->second;
        }
    }
    return folders;
}

void preserve_capture_timestamp(
    const std::filesystem::path& path,
    std::int64_t timestamp) {
    if (timestamp <= 0) return;
    try {
        const auto seconds_value =
            timestamp > 10'000'000'000LL ? timestamp / 1000 : timestamp;
        const auto seconds = std::chrono::seconds(seconds_value);
        const auto capture_time = std::chrono::system_clock::time_point(seconds);
#ifdef _WIN32
        constexpr std::uint64_t kUnixToWindowsEpochSeconds = 11'644'473'600ULL;
        if (seconds_value >= 0) {
            ULARGE_INTEGER raw_time{};
            raw_time.QuadPart =
                (static_cast<std::uint64_t>(seconds_value) +
                 kUnixToWindowsEpochSeconds) * 10'000'000ULL;
            FILETIME file_time{};
            file_time.dwLowDateTime = raw_time.LowPart;
            file_time.dwHighDateTime = raw_time.HighPart;
            HANDLE file = CreateFileW(
                path.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                SetFileTime(file, &file_time, nullptr, &file_time);
                CloseHandle(file);
            }
        }
#endif
        const auto file_time =
            std::filesystem::file_time_type::clock::now() +
            (capture_time - std::chrono::system_clock::now());
        std::filesystem::last_write_time(path, file_time);
    } catch (...) {
    }
}

}  // namespace

std::string SyncEngine::resolve_game_folder(
    const std::filesystem::path& album_directory,
    const std::string& app_name) const {
    const auto sanitized_name = sanitize_folder(app_name);
    if (!std::filesystem::exists(album_directory)) return sanitized_name;

    const auto normalized_app_name = normalize_title(app_name);
    std::vector<std::string> aliases{app_name};
    for (const auto& group : game_alias_groups()) {
        bool group_matches = false;
        for (const auto& alias : group) {
            if (normalize_title(alias) == normalized_app_name) {
                group_matches = true;
                break;
            }
        }
        if (group_matches) {
            aliases = group;
            break;
        }
    }

    for (const auto& directory : std::filesystem::directory_iterator(album_directory)) {
        if (!directory.is_directory()) continue;
        const auto existing_name = directory.path().filename().string();
        const auto normalized_existing = normalize_title(existing_name);
        for (const auto& alias : aliases) {
            const auto normalized_alias = normalize_title(alias);
            const bool exact_match = normalized_existing == normalized_alias;
            const bool fuzzy_match =
                normalized_existing.size() >= 6 && normalized_alias.size() >= 6 &&
                (normalized_existing.find(normalized_alias) != std::string::npos ||
                 normalized_alias.find(normalized_existing) != std::string::npos);
            if (exact_match || fuzzy_match) return existing_name;
        }
    }
    return sanitized_name;
}

SyncResult SyncEngine::sync(const std::function<bool()>& cancelled) {
    const auto config = config_.snapshot();
    if (config.session_token.empty()) {
        throw std::runtime_error("Not signed in to Nintendo Account");
    }

    throw_if_cancelled(cancelled);
    const auto media = coral_.media_list(config.session_token);
    throw_if_cancelled(cancelled);

    const std::filesystem::path root = config.destination_folder.empty()
        ? std::filesystem::current_path() / "Nintendo Switch Album"
        : std::filesystem::path(config.destination_folder);
    std::filesystem::create_directories(root);

    auto existing = index_existing_album(root, cancelled);
    const auto title_folders = learn_title_folders(media, existing);
    const bool root_is_album_directory = lower(root.filename().string()) == "album";
    const auto album_directory = root_is_album_directory ? root : root / "Album";

    int downloaded = 0;
    for (const auto& item : media) {
        throw_if_cancelled(cancelled);
        const auto timestamp = item.captured_at != 0 ? item.captured_at : item.uploaded_at;
        const auto prefix = capture_timestamp_prefix(timestamp);
        const auto extension = lower(item.type) == "video" ? "mp4" : "jpg";
        const auto filename = prefix + "_c." + extension;

        if (existing.filenames_and_prefixes.contains(lower(filename)) ||
            existing.filenames_and_prefixes.contains(lower(prefix))) {
            continue;
        }

        std::string game_folder;
        if (const auto known_folder = title_folders.find(item.title_id);
            known_folder != title_folders.end()) {
            game_folder = known_folder->second;
        } else {
            game_folder = resolve_game_folder(album_directory, item.app_name);
        }

        const auto destination = album_directory / game_folder / filename;
        std::filesystem::create_directories(destination.parent_path());

        const auto response = http_.get(item.content_uri, {}, 60);
        if (response.status / 100 != 2) {
            throw std::runtime_error(
                "Media download failed (HTTP " + std::to_string(response.status) + ")");
        }
        throw_if_cancelled(cancelled);

        auto temporary = destination;
        temporary += ".part";
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);

        try {
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file) throw std::runtime_error("Could not create media file");
                file.write(
                    reinterpret_cast<const char*>(response.body.data()),
                    static_cast<std::streamsize>(response.body.size()));
                file.flush();
                if (!file) throw std::runtime_error("Could not write media file");
            }
            throw_if_cancelled(cancelled);

            std::error_code rename_error;
            std::filesystem::rename(temporary, destination, rename_error);
            if (rename_error) {
                throw std::runtime_error(
                    "Could not finalize downloaded media: " + rename_error.message());
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }

        preserve_capture_timestamp(destination, timestamp);
        existing.filenames_and_prefixes.insert(lower(filename));
        existing.filenames_and_prefixes.insert(lower(prefix));
        ++downloaded;
    }

    return {static_cast<int>(media.size()), downloaded};
}

}  // namespace nso
