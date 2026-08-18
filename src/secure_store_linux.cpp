#include "nso_album_sync/secure_store.hpp"

#if defined(__linux__)

#ifdef NSO_HAVE_LIBSECRET
#include <libsecret/secret.h>

#include <string>

namespace nso {
namespace {

const SecretSchema kCredentialSchema = [] {
    SecretSchema schema{};
    schema.name = "org.nsoalbumsync.session-token";
    schema.flags = SECRET_SCHEMA_NONE;
    schema.attributes[0] = {
        "application",
        SECRET_SCHEMA_ATTRIBUTE_STRING,
    };
    schema.attributes[1] = {
        "key",
        SECRET_SCHEMA_ATTRIBUTE_STRING,
    };
    return schema;
}();

constexpr char kApplicationAttribute[] = "NsoAlbumSync";
constexpr char kLegacyKeyAttribute[] = "session_token";
constexpr char kNintendoAccountKey[] = "NintendoAccount";

std::string display_name(const std::string& account) {
    if (account == kNintendoAccountKey) {
        return "NSO Album Sync Nintendo Account session token";
    }
    if (account == "CoralCredential") {
        return "NSO Album Sync Coral credential cache";
    }
    return "NSO Album Sync credential";
}

std::optional<std::string> lookup_key(const std::string& key) {
    GError* error = nullptr;
    gchar* password = secret_password_lookup_sync(
        &kCredentialSchema,
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        key.c_str(),
        nullptr);

    if (error != nullptr) {
        g_error_free(error);
        return std::nullopt;
    }
    if (password == nullptr) return std::nullopt;

    std::string result(password);
    secret_password_free(password);
    return result;
}

void clear_key(const std::string& key) {
    GError* error = nullptr;
    secret_password_clear_sync(
        &kCredentialSchema,
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        key.c_str(),
        nullptr);
    if (error != nullptr) g_error_free(error);
}

}  // namespace

bool SecureStore::available() {
    GError* error = nullptr;
    auto* service = secret_service_get_sync(
        SECRET_SERVICE_OPEN_SESSION,
        nullptr,
        &error);

    if (error != nullptr) {
        g_error_free(error);
        return false;
    }
    if (service == nullptr) return false;

    g_object_unref(service);
    return true;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    if (account.empty()) return false;

    const auto label = display_name(account);
    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(
        &kCredentialSchema,
        SECRET_COLLECTION_DEFAULT,
        label.c_str(),
        secret.c_str(),
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        account.c_str(),
        nullptr);

    if (error != nullptr) g_error_free(error);
    if (stored == FALSE) return false;

    // Older Linux builds ignored the public `account` argument and used one
    // fixed `session_token` key. Migrate that slot only after the Nintendo
    // Account token has safely been written to its new account-specific key.
    if (account == kNintendoAccountKey) clear_key(kLegacyKeyAttribute);
    return true;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    if (account.empty()) return std::nullopt;
    if (const auto value = lookup_key(account)) return value;

    // Backward-compatible read of the one legacy slot. Never use this fallback
    // for CoralCredential: doing that would mistake a Nintendo session token for
    // a Coral cache entry and recreates the overwrite bug this migration fixes.
    if (account == kNintendoAccountKey) return lookup_key(kLegacyKeyAttribute);
    return std::nullopt;
}

void SecureStore::erase(const std::string& account) {
    if (account.empty()) return;
    clear_key(account);
    if (account == kNintendoAccountKey) clear_key(kLegacyKeyAttribute);
}

}  // namespace nso

#else

namespace nso {

bool SecureStore::available() {
    return false;
}

bool SecureStore::put(const std::string&, const std::string&) {
    return false;
}

std::optional<std::string> SecureStore::get(const std::string&) {
    return std::nullopt;
}

void SecureStore::erase(const std::string&) {}

}  // namespace nso

#endif  // NSO_HAVE_LIBSECRET
#endif  // __linux__
