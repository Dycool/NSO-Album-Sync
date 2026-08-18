#include "nso_album_sync/sync.hpp"

#include "nso_album_sync/game_aliases.hpp"
#include "nso_album_sync/util.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#endif

#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <set>
#include <utility>
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

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::pair<std::uint32_t, std::size_t> decode_utf8(
    const std::string& text,
    std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};

    auto continuation = [&](std::size_t index) -> int {
        if (index >= text.size()) return -1;
        const auto byte = static_cast<unsigned char>(text[index]);
        return (byte & 0xc0) == 0x80 ? (byte & 0x3f) : -1;
    };

    if ((first & 0xe0) == 0xc0) {
        const int b1 = continuation(offset + 1);
        if (b1 >= 0) return {((first & 0x1fU) << 6U) | static_cast<unsigned>(b1), 2};
    } else if ((first & 0xf0) == 0xe0) {
        const int b1 = continuation(offset + 1);
        const int b2 = continuation(offset + 2);
        if (b1 >= 0 && b2 >= 0) {
            return {((first & 0x0fU) << 12U) |
                        (static_cast<unsigned>(b1) << 6U) |
                        static_cast<unsigned>(b2),
                    3};
        }
    } else if ((first & 0xf8) == 0xf0) {
        const int b1 = continuation(offset + 1);
        const int b2 = continuation(offset + 2);
        const int b3 = continuation(offset + 3);
        if (b1 >= 0 && b2 >= 0 && b3 >= 0) {
            return {((first & 0x07U) << 18U) |
                        (static_cast<unsigned>(b1) << 12U) |
                        (static_cast<unsigned>(b2) << 6U) |
                        static_cast<unsigned>(b3),
                    4};
        }
    }
    return {first, 1};
}

std::string normalize_for_matching_v1(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (std::size_t offset = 0; offset < text.size();) {
        const auto [code_point, width] = decode_utf8(text, offset);
        offset += width;

        if (code_point >= 0x0300 && code_point <= 0x036f) continue;

        if (code_point < 0x80) {
            const auto character = static_cast<unsigned char>(code_point);
            if (std::isalnum(character)) {
                normalized.push_back(static_cast<char>(std::tolower(character)));
            }
            continue;
        }

        char latin = '\0';
        switch (code_point) {
            case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x00c4: case 0x00c5:
            case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x00e4: case 0x00e5:
                latin = 'a'; break;
            case 0x00c7: case 0x00e7: latin = 'c'; break;
            case 0x00c8: case 0x00c9: case 0x00ca: case 0x00cb:
            case 0x00e8: case 0x00e9: case 0x00ea: case 0x00eb:
                latin = 'e'; break;
            case 0x00cc: case 0x00cd: case 0x00ce: case 0x00cf:
            case 0x00ec: case 0x00ed: case 0x00ee: case 0x00ef:
                latin = 'i'; break;
            case 0x00d1: case 0x00f1: latin = 'n'; break;
            case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x00d6: case 0x00d8:
            case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x00f6: case 0x00f8:
                latin = 'o'; break;
            case 0x00d9: case 0x00da: case 0x00db: case 0x00dc:
            case 0x00f9: case 0x00fa: case 0x00fb: case 0x00fc:
                latin = 'u'; break;
            case 0x00dd: case 0x0178: case 0x00fd: case 0x00ff:
                latin = 'y'; break;
            default: break;
        }
        if (latin != '\0') {
            normalized.push_back(latin);
            continue;
        }

        // .NET v1 kept non-Latin letters/digits after Unicode decomposition.
        // Preserve their UTF-8 representation so exact Japanese and other
        // localized aliases continue to compare instead of collapsing to empty.
        append_utf8(normalized, code_point);
    }
    return normalized;
}

std::string sanitize_folder_v1(const std::string& text) {
    if (trim(text).empty()) return "Other";
    std::string clean;
    clean.reserve(text.size());
    for (const unsigned char character : text) {
        if (character < 0x20) continue;
        switch (character) {
            case '<': case '>': case ':': case '"': case '/':
            case '\\': case '|': case '?': case '*':
                continue;
            default:
                clean.push_back(static_cast<char>(character));
                break;
        }
    }
    clean = trim(std::move(clean));
    return clean.empty() ? "Other" : clean;
}

bool equal_v1_name(const std::string& left, const std::string& right) {
    return lower(left) == lower(right);
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
            folders[lower(item.title_id)] = existing_folder->second;
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
    const auto default_clean = sanitize_folder_v1(app_name);
    if (!std::filesystem::exists(album_directory) || trim(app_name).empty()) {
        return default_clean;
    }

    try {
        struct DirectoryName {
            std::string original;
            std::string normalized;
        };
        std::vector<DirectoryName> directories;
        for (const auto& directory : std::filesystem::directory_iterator(album_directory)) {
            if (!directory.is_directory()) continue;
            const auto name = directory.path().filename().string();
            directories.push_back({name, normalize_for_matching_v1(name)});
        }

        const auto normalized_app_name = normalize_for_matching_v1(app_name);
        std::vector<std::string> synonyms{app_name};
        for (const auto& group : game_alias_groups()) {
            bool group_matches = false;
            for (const auto& alias : group) {
                if (normalize_for_matching_v1(alias) == normalized_app_name) {
                    group_matches = true;
                    break;
                }
            }
            if (group_matches) {
                synonyms = group;
                break;
            }
        }

        // Match v1.0.0 ordering: sanitized exact match first, then normalized
        // exact match for every known localization/synonym.
        for (const auto& synonym : synonyms) {
            const auto clean_synonym = sanitize_folder_v1(synonym);
            const auto normalized_synonym = normalize_for_matching_v1(synonym);

            for (const auto& directory : directories) {
                if (equal_v1_name(directory.original, clean_synonym)) {
                    return directory.original;
                }
            }
            for (const auto& directory : directories) {
                if (directory.normalized == normalized_synonym) {
                    return directory.original;
                }
            }
        }

        // v1's final fallback only fuzzed the API title itself, not every alias.
        if (!normalized_app_name.empty()) {
            for (const auto& directory : directories) {
                if (directory.normalized.size() >= 6 &&
                    normalized_app_name.size() >= 6 &&
                    (directory.normalized.find(normalized_app_name) != std::string::npos ||
                     normalized_app_name.find(directory.normalized) != std::string::npos)) {
                    return directory.original;
                }
            }
        }
    } catch (...) {
        // v1 treated local folder enumeration/matching failures as non-fatal and
        // simply fell back to the sanitized API title.
    }

    return default_clean;
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
        if (const auto known_folder = title_folders.find(lower(item.title_id));
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
