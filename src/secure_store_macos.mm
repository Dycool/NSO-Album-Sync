#include "nso_album_sync/secure_store.hpp"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nso {
namespace {

NSString* const kKeychainService = @"org.nsoalbumsync.session-token";
constexpr std::size_t kMaxLegacySecretBytes = 64 * 1024;

NSDictionary* keychain_query(NSString* account) {
    return @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : kKeychainService,
        (id)kSecAttrAccount : account,
    };
}

std::filesystem::path legacy_credentials_directory() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
        "Library" / "Application Support" / "NSOAlbumSync" / "credentials";
}

std::string safe_account_name(const std::string& account) {
    std::string safe;
    safe.reserve(account.size());
    for (const unsigned char ch : account) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            safe.push_back(static_cast<char>(ch));
        } else {
            safe.push_back('_');
        }
    }
    return safe.empty() ? std::string("credential") : safe;
}

std::filesystem::path legacy_credential_path(const std::string& account) {
    return legacy_credentials_directory() / (safe_account_name(account) + ".dat");
}

void remove_legacy_credential(const std::string& account) {
    std::error_code error;
    std::filesystem::remove(legacy_credential_path(account), error);
    error.clear();
    std::filesystem::remove(legacy_credentials_directory(), error);
}

std::optional<std::string> read_legacy_credential(const std::string& account) {
    const auto path = legacy_credential_path(account);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;

    struct stat info {};
    if (fstat(fd, &info) != 0 ||
        !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() ||
        (info.st_mode & 0077) != 0 ||
        info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > kMaxLegacySecretBytes) {
        close(fd);
        return std::nullopt;
    }

    std::string secret(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < secret.size()) {
        const auto amount = read(fd, secret.data() + offset, secret.size() - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) {
            close(fd);
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(amount);
    }
    close(fd);
    return secret;
}

bool keychain_put(const std::string& account, const std::string& secret) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        if (account_string == nil) return false;

        NSData* secret_data = [NSData
            dataWithBytes:secret.data()
                   length:secret.size()];
        NSDictionary* query = keychain_query(account_string);
        NSDictionary* update = @{(id)kSecValueData : secret_data};

        OSStatus status = SecItemUpdate(
            (CFDictionaryRef)query,
            (CFDictionaryRef)update);
        if (status == errSecItemNotFound) {
            NSMutableDictionary* item = [NSMutableDictionary
                dictionaryWithDictionary:query];
            item[(id)kSecValueData] = secret_data;
            status = SecItemAdd((CFDictionaryRef)item, nullptr);
        }
        return status == errSecSuccess;
    }
}

std::optional<std::string> keychain_get(const std::string& account) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        if (account_string == nil) return std::nullopt;

        NSMutableDictionary* query = [NSMutableDictionary
            dictionaryWithDictionary:keychain_query(account_string)];
        query[(id)kSecReturnData] = @YES;
        query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(
            (CFDictionaryRef)query,
            &result);
        if (status != errSecSuccess || result == nullptr) {
            if (result != nullptr) CFRelease(result);
            return std::nullopt;
        }

        NSData* data = (NSData*)result;
        const auto* bytes = static_cast<const char*>(data.bytes);
        std::string secret(bytes, bytes + data.length);
        CFRelease(result);
        return secret;
    }
}

std::optional<std::string> migrate_legacy_credential(const std::string& account) {
    const auto legacy = read_legacy_credential(account);
    if (!legacy) {
        // A malformed, over-permissive, or symlinked development credential is
        // not safe to retain. Remove it and require a fresh sign-in instead.
        remove_legacy_credential(account);
        return std::nullopt;
    }

    // Temporary development builds stored these secrets as plaintext files.
    // Import once into Keychain, then remove the plaintext copy. If Keychain
    // rejects the import, remove the unsafe copy anyway and require sign-in
    // again rather than silently continuing insecure persistence.
    const bool stored = keychain_put(account, *legacy);
    remove_legacy_credential(account);
    return stored ? legacy : std::nullopt;
}

void migrate_known_legacy_credentials() {
    for (const char* account : {"NintendoAccount", "CoralCredential"}) {
        if (keychain_get(account)) {
            remove_legacy_credential(account);
        } else {
            (void)migrate_legacy_credential(account);
        }
    }
}

}  // namespace

bool SecureStore::available() {
    static const bool migrated = [] {
        migrate_known_legacy_credentials();
        return true;
    }();
    (void)migrated;
    return true;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    const bool stored = keychain_put(account, secret);
    // Never leave a plaintext development-store duplicate behind, regardless
    // of whether the new Keychain write succeeds.
    remove_legacy_credential(account);
    return stored;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    if (const auto secret = keychain_get(account)) {
        remove_legacy_credential(account);
        return secret;
    }
    return migrate_legacy_credential(account);
}

void SecureStore::erase(const std::string& account) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        if (account_string != nil) {
            SecItemDelete((CFDictionaryRef)keychain_query(account_string));
        }
    }
    remove_legacy_credential(account);
}

}  // namespace nso

#endif  // __APPLE__
