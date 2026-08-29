#include "nso_album_sync/secure_store.hpp"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace nso {
namespace {

NSString* const kKeychainService = @"org.nsoalbumsync.session-token";

NSDictionary* keychain_query(NSString* account) {
    return @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : kKeychainService,
        (id)kSecAttrAccount : account,
    };
}

}  // namespace

bool SecureStore::available() {
    return true;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        NSData* secret_data = [NSData
            dataWithBytes:secret.data()
                   length:secret.size()];

        // Delete the previous value first so SecItemAdd has simple, predictable
        // semantics and we never leave more than one token for this account.
        SecItemDelete((CFDictionaryRef)keychain_query(account_string));

        NSMutableDictionary* item = [NSMutableDictionary
            dictionaryWithDictionary:keychain_query(account_string)];
        item[(id)kSecValueData] = secret_data;

        return SecItemAdd(
                   (CFDictionaryRef)item,
                   nullptr) == errSecSuccess;
    }
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        NSMutableDictionary* query = [NSMutableDictionary
            dictionaryWithDictionary:keychain_query(account_string)];

        query[(id)kSecReturnData] = @YES;
        query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(
            (CFDictionaryRef)query,
            &result);

        if (status != errSecSuccess || result == nullptr) {
            if (result != nullptr) {
                CFRelease(result);
            }
            return std::nullopt;
        }

        NSData* data = (NSData*)result;
        std::string secret(
            static_cast<const char*>(data.bytes),
            static_cast<const char*>(data.bytes) + data.length);
        CFRelease(result);
        return secret;
    }
}

void SecureStore::erase(const std::string& account) {
    @autoreleasepool {
        NSString* account_string = [NSString stringWithUTF8String:account.c_str()];
        SecItemDelete((CFDictionaryRef)keychain_query(account_string));
    }
}

}  // namespace nso

#endif  // __APPLE__
