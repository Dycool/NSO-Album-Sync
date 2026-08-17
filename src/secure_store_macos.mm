#include "nso_album_sync/secure_store.hpp"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cstring>

namespace nso {
namespace {

constexpr char kKeychainService[] = "org.nsoalbumsync.session-token";

}  // namespace

bool SecureStore::available() {
    return true;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    // SecKeychainAddGenericPassword does not update an existing item.
    erase(account);

    return SecKeychainAddGenericPassword(
               nullptr,
               static_cast<UInt32>(std::strlen(kKeychainService)),
               kKeychainService,
               static_cast<UInt32>(account.size()),
               account.data(),
               static_cast<UInt32>(secret.size()),
               secret.data(),
               nullptr) == errSecSuccess;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    UInt32 secret_length = 0;
    void* secret_data = nullptr;

    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(std::strlen(kKeychainService)),
        kKeychainService,
        static_cast<UInt32>(account.size()),
        account.data(),
        &secret_length,
        &secret_data,
        nullptr);

    if (status != errSecSuccess) {
        return std::nullopt;
    }

    std::string secret(
        static_cast<char*>(secret_data),
        static_cast<char*>(secret_data) + secret_length);

    SecKeychainItemFreeContent(nullptr, secret_data);
    return secret;
}

void SecureStore::erase(const std::string& account) {
    SecKeychainItemRef item = nullptr;

    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(std::strlen(kKeychainService)),
        kKeychainService,
        static_cast<UInt32>(account.size()),
        account.data(),
        nullptr,
        nullptr,
        &item);

    if (status == errSecSuccess) {
        SecKeychainItemDelete(item);
        CFRelease(item);
    }
}

}  // namespace nso

#endif  // __APPLE__
