#include "nso_album_sync/secure_store.hpp"

#if defined(__linux__)

#ifdef NSO_HAVE_LIBSECRET
#include <libsecret/secret.h>

namespace nso {
namespace {

const SecretSchema kSessionTokenSchema = [] {
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
constexpr char kDisplayName[] = "NSO Album Sync Nintendo session token";

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

    if (service != nullptr) {
        g_object_unref(service);
        return true;
    }

    return false;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    // Older Linux builds used a fixed key attribute. Keep it for migration and
    // compatibility even though the public SecureStore API accepts an account.
    (void)account;

    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(
        &kSessionTokenSchema,
        SECRET_COLLECTION_DEFAULT,
        kDisplayName,
        secret.c_str(),
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        kLegacyKeyAttribute,
        nullptr);

    if (error != nullptr) {
        g_error_free(error);
    }

    return stored != FALSE;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    (void)account;

    GError* error = nullptr;
    gchar* password = secret_password_lookup_sync(
        &kSessionTokenSchema,
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        kLegacyKeyAttribute,
        nullptr);

    if (error != nullptr) {
        g_error_free(error);
        return std::nullopt;
    }

    if (password == nullptr) {
        return std::nullopt;
    }

    std::string result(password);
    secret_password_free(password);
    return result;
}

void SecureStore::erase(const std::string& account) {
    (void)account;

    GError* error = nullptr;
    secret_password_clear_sync(
        &kSessionTokenSchema,
        nullptr,
        &error,
        "application",
        kApplicationAttribute,
        "key",
        kLegacyKeyAttribute,
        nullptr);

    if (error != nullptr) {
        g_error_free(error);
    }
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
