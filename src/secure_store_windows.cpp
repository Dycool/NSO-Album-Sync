#include "nso_album_sync/secure_store.hpp"

#ifdef _WIN32

#include <windows.h>
#include <wincred.h>

#include <string>

#pragma comment(lib, "Advapi32.lib")

namespace nso {
namespace {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        nullptr,
        0);

    std::wstring result(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        result.data(),
        required);

    return result;
}

std::wstring credential_target(const std::string& account) {
    return L"Dycool.NSOAlbumSync/" + utf8_to_wide(account);
}

}  // namespace

bool SecureStore::available() {
    return true;
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    auto target = credential_target(account);

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(secret.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"Nintendo Account");

    return CredWriteW(&credential, 0) != FALSE;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    const auto target = credential_target(account);
    PCREDENTIALW credential = nullptr;

    if (!CredReadW(
            target.c_str(),
            CRED_TYPE_GENERIC,
            0,
            &credential)) {
        return std::nullopt;
    }

    std::string secret(
        reinterpret_cast<char*>(credential->CredentialBlob),
        reinterpret_cast<char*>(credential->CredentialBlob) +
            credential->CredentialBlobSize);

    CredFree(credential);
    return secret;
}

void SecureStore::erase(const std::string& account) {
    const auto target = credential_target(account);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

}  // namespace nso

#endif  // _WIN32
