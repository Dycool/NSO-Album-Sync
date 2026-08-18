#include "nso_album_sync/config.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#else
#include "nso_album_sync/windows_compat.hpp"
#include <shlobj.h>
#include <wincrypt.h>
#endif

namespace nso {
namespace {

constexpr char kSecureStoreAccount[] = "NintendoAccount";
constexpr char kSecureMarker[] = "secure:v1";
constexpr char kMacKeychainMarker[] = "keychain:v1";
constexpr char kLinuxSecretServiceMarker[] = "secret-service:v1";
constexpr char kVolatileMarker[] = "volatile:v1";

std::string environment_variable(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::filesystem::path config_directory() {
#ifdef _WIN32
    const auto app_data = environment_variable("APPDATA");
    return std::filesystem::path(app_data.empty() ? "." : app_data) / "NSOAlbumSync";
#elif __APPLE__
    const auto home = environment_variable("HOME");
    return std::filesystem::path(home.empty() ? "." : home) /
           "Library" / "Application Support" / "NSOAlbumSync";
#else
    const auto xdg_config_home = environment_variable("XDG_CONFIG_HOME");
    if (!xdg_config_home.empty()) {
        return std::filesystem::path(xdg_config_home) / "NSOAlbumSync";
    }
    const auto home = environment_variable("HOME");
    return std::filesystem::path(home.empty() ? "." : home) /
           ".config" / "NSOAlbumSync";
#endif
}
}  // namespace

std::string default_album_folder() {
#ifdef _WIN32
    wchar_t pictures[MAX_PATH]{};
    wchar_t videos[MAX_PATH]{};
    const bool has_pictures = SUCCEEDED(SHGetFolderPathW(
        nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures));
    const bool has_videos = SUCCEEDED(SHGetFolderPathW(
        nullptr, CSIDL_MYVIDEO, nullptr, SHGFP_TYPE_CURRENT, videos));

    std::vector<std::filesystem::path> candidates;
    if (has_videos && videos[0] != L'\0') {
        const std::filesystem::path v(videos);
        candidates.push_back(v / L"Nintendo Switch 2" / L"Album");
        candidates.push_back(v / L"Nintendo Switch" / L"Album");
        candidates.push_back(v / L"Nintendo Switch 2");
        candidates.push_back(v / L"Nintendo Switch");
    }
    if (has_pictures && pictures[0] != L'\0') {
        const std::filesystem::path p(pictures);
        candidates.push_back(p / L"Nintendo Switch 2" / L"Album");
        candidates.push_back(p / L"Nintendo Switch" / L"Album");
        candidates.push_back(p / L"Nintendo Switch 2");
        candidates.push_back(p / L"Nintendo Switch");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate.string();
        }
    }

    if (has_pictures && pictures[0] != L'\0') {
        return (std::filesystem::path(pictures) / L"Nintendo Switch").string();
    }
    const auto profile = environment_variable("USERPROFILE");
    return (std::filesystem::path(profile.empty() ? "." : profile) /
            "Pictures" / "Nintendo Switch").string();
#elif __APPLE__
    const auto home = environment_variable("HOME");
    const std::filesystem::path base = home.empty() ? "." : home;
    const auto candidate = base / "Pictures" / "Nintendo Switch" / "Album";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
        return candidate.string();
    }
    return (base / "Pictures" / "Nintendo Switch").string();
#else
    const auto home = environment_variable("HOME");
    const std::filesystem::path base = home.empty() ? "." : home;
    const auto candidate = base / "Pictures" / "Nintendo Switch" / "Album";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
        return candidate.string();
    }
    return (base / "Pictures" / "Nintendo Switch").string();
#endif
}

namespace {

std::string string_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    const std::string& fallback = {}) {
    const auto current = json.string(current_key);
    return current.empty() ? json.string(legacy_key, fallback) : current;
}

bool bool_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    bool fallback) {
    if (const auto* current = json.find(current_key);
        current != nullptr && current->is_bool()) {
        return current->as_bool();
    }
    return json.boolean(legacy_key, fallback);
}

std::int64_t integer_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    std::int64_t fallback) {
    if (const auto* current = json.find(current_key);
        current != nullptr && current->is_number()) {
        return current->as_i64();
    }
    return json.integer(legacy_key, fallback);
}

std::string display_last_sync(const std::string& stored) {
    if (stored.empty()) return "Never";
    if (stored.size() >= 16 && stored[4] == '-' && stored[7] == '-' &&
        (stored[10] == 'T' || stored[10] == ' ')) {
        return stored.substr(11, 5) + " (" + stored.substr(0, 10) + ")";
    }
    return stored;
}

bool is_secure_store_marker(const std::string& value) {
    return value == kSecureMarker || value == kMacKeychainMarker ||
           value == kLinuxSecretServiceMarker;
}

#ifdef _WIN32
std::string decrypt_legacy_dpapi_token(const std::string& value) {
    constexpr char kPrefix[] = "dpapi:";
    if (value.rfind(kPrefix, 0) != 0) return {};

    const auto cipher = base64_decode(value.substr(sizeof(kPrefix) - 1));
    if (cipher.empty()) return {};

    static constexpr char kEntropy[] = "NSO_Album_Sync_Salt_9981";
    DATA_BLOB input{
        static_cast<DWORD>(cipher.size()),
        const_cast<BYTE*>(cipher.data()),
    };
    DATA_BLOB entropy{
        static_cast<DWORD>(sizeof(kEntropy) - 1),
        reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)),
    };
    DATA_BLOB output{};

    if (!CryptUnprotectData(
            &input, nullptr, &entropy, nullptr, nullptr, 0, &output)) {
        return {};
    }

    std::string plain(
        reinterpret_cast<char*>(output.pbData),
        reinterpret_cast<char*>(output.pbData) + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
}
#endif

void replace_config_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not replace config.json");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("Could not replace config.json");
    }
#endif
}

}  // namespace

ConfigManager::ConfigManager()
    : directory_(config_directory()),
      config_file_(directory_ / "config.json") {
    load();
}

AppConfig ConfigManager::snapshot() const {
    std::lock_guard lock(mutex_);
    return config_;
}

AppConfig ConfigManager::update(const std::function<void(AppConfig&)>& updater) {
    std::lock_guard lock(mutex_);
    updater(config_);
    save_locked();
    return config_;
}

void ConfigManager::load() {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);
#ifndef _WIN32
    chmod(directory_.c_str(), 0700);
#endif

    bool needs_config_rewrite = false;

    if (std::filesystem::exists(config_file_)) {
        try {
            std::ifstream file(config_file_);
            const std::string contents{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()};
            const auto json = Json::parse(contents);

            config_.user_nickname = string_with_legacy_key(
                json, "userNickname", "UserNickname", config_.user_nickname);
            config_.destination_folder = string_with_legacy_key(
                json, "destinationFolder", "DestinationFolder", config_.destination_folder);
            // v1.0.0 defaulted auto-sync to enabled and respected an
            // explicitly saved AutoSyncEnabled/autoSync value. Preserve that
            // behavior across the C++ config migration.
            config_.auto_sync = bool_with_legacy_key(
                json, "autoSync", "AutoSyncEnabled", config_.auto_sync);
            config_.auto_sync_setting_version = 1;
            config_.notifications = bool_with_legacy_key(
                json, "notifications", "NotificationsEnabled", false);
            const auto discord_setting_version =
                json.integer("discordPresenceSettingVersion", 0);
            if (discord_setting_version >= 1) {
                config_.discord_presence = bool_with_legacy_key(
                    json, "discordPresence", "DiscordPresenceEnabled", false);
            } else {
                // Older v2 builds enabled RPC by default. Treat those saved
                // defaults as not having explicit consent and require one
                // deliberate opt-in after upgrading.
                config_.discord_presence = false;
                needs_config_rewrite = true;
            }
            config_.discord_presence_setting_version = 1;
            config_.start_on_boot = bool_with_legacy_key(
                json, "startOnBoot", "StartOnBoot", false);
            config_.sync_interval_minutes = static_cast<int>(std::clamp<std::int64_t>(
                integer_with_legacy_key(
                    json, "syncIntervalMinutes", "SyncIntervalMinutes", 60),
                1,
                (std::numeric_limits<int>::max)()));
            config_.last_sync = display_last_sync(string_with_legacy_key(
                json, "lastSync", "LastSyncTime", "Never"));
            config_.proxy_url = string_with_legacy_key(
                json, "proxyUrl", "ProxyUrl");
            config_.nxapi_auth_client_id = string_with_legacy_key(
                json, "nxapiAuthClientId", "NxapiAuthClientId",
                config_.nxapi_auth_client_id);

            // Rich Presence uses one public application identity controlled by
            // NSO Album Sync. Older builds persisted this 64-bit snowflake as a
            // JSON number, which can lose precision because Json stores numbers
            // as doubles. Ignore and remove any legacy override instead.
            config_.discord_application_id = kDiscordApplicationId;
            if (json.find("discordApplicationId") != nullptr ||
                json.find("DiscordApplicationId") != nullptr) {
                needs_config_rewrite = true;
            }

            const auto stored_session = string_with_legacy_key(
                json, "sessionToken", "SessionToken");

            if (is_secure_store_marker(stored_session)) {
                if (const auto token = SecureStore::get(kSecureStoreAccount)) {
                    config_.session_token = *token;
                }
            } else if (stored_session.empty() || stored_session == kVolatileMarker) {
                config_.session_token.clear();
#ifdef _WIN32
            } else if (stored_session.rfind("dpapi:", 0) == 0) {
                config_.session_token = decrypt_legacy_dpapi_token(stored_session);
                if (!config_.session_token.empty() &&
                    SecureStore::put(kSecureStoreAccount, config_.session_token)) {
                    needs_config_rewrite = true;
                }
#endif
            } else {
                config_.session_token = stored_session;
                if (SecureStore::available() &&
                    SecureStore::put(kSecureStoreAccount, stored_session)) {
                    needs_config_rewrite = true;
                }
            }
        } catch (...) {
            // Keep defaults if the config is corrupt or from an unknown version.
        }
    }

    // Keep a corrupt/missing config from silently falling back to the process
    // working directory, which can otherwise scatter an Album folder beside the exe.
    if (config_.destination_folder.empty()) {
        config_.destination_folder = default_album_folder();
    }

    if (needs_config_rewrite) {
        save_locked();
    }
}

void ConfigManager::save_locked() {
    std::filesystem::create_directories(directory_);
#ifndef _WIN32
    chmod(directory_.c_str(), 0700);
#endif

    std::string session_marker;
    if (!config_.session_token.empty()) {
        const bool stored_securely =
            SecureStore::available() &&
            SecureStore::put(kSecureStoreAccount, config_.session_token);
        session_marker = stored_securely ? kSecureMarker : kVolatileMarker;
    }

    const Json json(Json::object{
        {"sessionToken", session_marker},
        {"userNickname", config_.user_nickname},
        {"destinationFolder", config_.destination_folder},
        {"autoSync", config_.auto_sync},
        {"autoSyncSettingVersion",
         static_cast<std::int64_t>(config_.auto_sync_setting_version)},
        {"notifications", config_.notifications},
        {"discordPresence", config_.discord_presence},
        {"discordPresenceSettingVersion",
         static_cast<std::int64_t>(config_.discord_presence_setting_version)},
        {"startOnBoot", config_.start_on_boot},
        {"syncIntervalMinutes", config_.sync_interval_minutes},
        {"lastSync", config_.last_sync},
        {"proxyUrl", config_.proxy_url},
        {"nxapiAuthClientId", config_.nxapi_auth_client_id},
    });

    auto temporary = config_file_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc | std::ios::binary);
        if (!file) throw std::runtime_error("Could not write config.json");
        file << json.dump();
        file.flush();
        if (!file) throw std::runtime_error("Could not write config.json");
    }
#ifndef _WIN32
    chmod(temporary.c_str(), 0600);
#endif
    replace_config_file(temporary, config_file_);
#ifndef _WIN32
    chmod(config_file_.c_str(), 0600);
#endif
}

void ConfigManager::save() {
    std::lock_guard lock(mutex_);
    save_locked();
}

void ConfigManager::clear_session() {
    std::lock_guard lock(mutex_);
    config_.session_token.clear();
    SecureStore::erase(kSecureStoreAccount);
    save_locked();
}

}  // namespace nso
