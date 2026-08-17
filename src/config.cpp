#include "nso_album_sync/config.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/util.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

#ifndef _WIN32
#include <sys/stat.h>
#else
#include "nso_album_sync/windows_compat.hpp"
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
    return std::filesystem::path(app_data.empty() ? "." : app_data) /
           "NSOAlbumSync";
#elif __APPLE__
    const auto home = environment_variable("HOME");
    return std::filesystem::path(home.empty() ? "." : home) /
           "Library" /
           "Application Support" /
           "NSOAlbumSync";
#else
    const auto xdg_config_home = environment_variable("XDG_CONFIG_HOME");
    if (!xdg_config_home.empty()) {
        return std::filesystem::path(xdg_config_home) / "NSOAlbumSync";
    }

    const auto home = environment_variable("HOME");
    return std::filesystem::path(home.empty() ? "." : home) /
           ".config" /
           "NSOAlbumSync";
#endif
}

std::string default_album_folder() {
    const auto home = environment_variable("HOME");
    return (std::filesystem::path(home.empty() ? "." : home) /
            "Pictures" /
            "Nintendo Switch Album")
        .string();
}

std::string string_with_legacy_key(
    const Json& json,
    const char* current_key,
    const char* legacy_key,
    const std::string& fallback = {}) {
    const auto current = json.string(current_key);
    return current.empty()
        ? json.string(legacy_key, fallback)
        : current;
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

bool is_secure_store_marker(const std::string& value) {
    return value == kSecureMarker ||
           value == kMacKeychainMarker ||
           value == kLinuxSecretServiceMarker;
}

#ifdef _WIN32
std::string decrypt_legacy_dpapi_token(const std::string& value) {
    constexpr char kPrefix[] = "dpapi:";
    if (value.rfind(kPrefix, 0) != 0) {
        return {};
    }

    const auto cipher = base64_decode(value.substr(sizeof(kPrefix) - 1));
    if (cipher.empty()) {
        return {};
    }

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
            &input,
            nullptr,
            &entropy,
            nullptr,
            nullptr,
            0,
            &output)) {
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

}  // namespace

ConfigManager::ConfigManager()
    : directory_(config_directory()),
      config_file_(directory_ / "config.json") {
    load();
}

void ConfigManager::load() {
    std::unique_lock lock(mutex_);
    std::filesystem::create_directories(directory_);

#ifndef _WIN32
    chmod(directory_.c_str(), 0700);
#endif

    if (!std::filesystem::exists(config_file_)) {
        config_.destination_folder = default_album_folder();
        return;
    }

    bool needs_secure_store_migration = false;

    try {
        std::ifstream file(config_file_);
        const std::string contents{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        const auto json = Json::parse(contents);

        config_.user_nickname = string_with_legacy_key(
            json,
            "userNickname",
            "UserNickname",
            config_.user_nickname);

        config_.destination_folder = string_with_legacy_key(
            json,
            "destinationFolder",
            "DestinationFolder",
            config_.destination_folder);

        config_.auto_sync = bool_with_legacy_key(
            json,
            "autoSync",
            "AutoSyncEnabled",
            true);

        config_.notifications = bool_with_legacy_key(
            json,
            "notifications",
            "NotificationsEnabled",
            false);

        config_.discord_presence = bool_with_legacy_key(
            json,
            "discordPresence",
            "DiscordPresenceEnabled",
            true);

        config_.proxy_url = string_with_legacy_key(
            json,
            "proxyUrl",
            "ProxyUrl");

        config_.nxapi_auth_client_id = string_with_legacy_key(
            json,
            "nxapiAuthClientId",
            "NxapiAuthClientId",
            config_.nxapi_auth_client_id);

        config_.discord_application_id = static_cast<std::uint64_t>(
            json.integer(
                "discordApplicationId",
                static_cast<std::int64_t>(config_.discord_application_id)));

        const auto stored_session = string_with_legacy_key(
            json,
            "sessionToken",
            "SessionToken");

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
                needs_secure_store_migration = true;
            }
#endif
        } else {
            // Legacy builds could leave a plaintext session token in config.json.
            // Load it once, then migrate it into the OS credential store.
            config_.session_token = stored_session;

            if (SecureStore::available() &&
                SecureStore::put(kSecureStoreAccount, stored_session)) {
                needs_secure_store_migration = true;
            }
        }

        if (config_.destination_folder.empty()) {
            config_.destination_folder = default_album_folder();
        }
    } catch (...) {
        // Keep defaults if the config is corrupt or from an unknown version.
    }

    if (needs_secure_store_migration) {
        lock.unlock();
        save();
    }
}

void ConfigManager::save() {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(directory_);

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
        {"notifications", config_.notifications},
        {"discordPresence", config_.discord_presence},
        {"startOnBoot", config_.start_on_boot},
        {"proxyUrl", config_.proxy_url},
        {"nxapiAuthClientId", config_.nxapi_auth_client_id},
        {"discordApplicationId",
         static_cast<std::int64_t>(config_.discord_application_id)},
    });

    std::ofstream file(config_file_, std::ios::trunc);
    file << json.dump();
    file.close();

#ifndef _WIN32
    chmod(config_file_.c_str(), 0600);
#endif
}

void ConfigManager::clear_session() {
    config_.session_token.clear();
    SecureStore::erase(kSecureStoreAccount);
    save();
}

}  // namespace nso
