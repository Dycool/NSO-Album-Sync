#include "nso_album_sync/auth_callback.hpp"

#ifdef _WIN32

#include "nso_album_sync/windows_compat.hpp"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr wchar_t kSchemeKey[] = L"npf71b963c1b7b6d119";
constexpr wchar_t kUserSchemeKey[] =
    L"Software\\Classes\\npf71b963c1b7b6d119";
constexpr wchar_t kCommandSuffix[] = L"\\shell\\open\\command";
constexpr wchar_t kHandlerDescription[] =
    L"URL:NSO Album Sync Nintendo Account callback";
constexpr wchar_t kTrayClass[] = L"NSOAlbumSyncTray";
constexpr ULONG_PTR kAuthCallbackCopyDataId = 0x4E534F41u;

bool g_created_handler = false;

std::wstring executable_path() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring desired_command() {
    const auto path = executable_path();
    if (path.empty()) {
        return {};
    }
    return L"\"" + path + L"\" \"%1\"";
}

std::wstring read_default_value(HKEY root, const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(
            key, nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(
            key,
            nullptr,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    return std::wstring(buffer.data());
}

bool equal_case_insensitive(std::wstring left, std::wstring right) {
    const auto lower = [](std::wstring& value) {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    };
    lower(left);
    lower(right);
    return left == right;
}

bool write_string_value(
    HKEY key,
    const wchar_t* name,
    const std::wstring& value) {
    const auto bytes = static_cast<DWORD>(
        (value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(
               key,
               name,
               0,
               REG_SZ,
               reinterpret_cast<const BYTE*>(value.c_str()),
               bytes) == ERROR_SUCCESS;
}

bool current_handler_is_ours() {
    const auto command = read_default_value(
        HKEY_CLASSES_ROOT,
        std::wstring(kSchemeKey) + kCommandSuffix);
    const auto desired = desired_command();
    return !command.empty() && !desired.empty() &&
           equal_case_insensitive(command, desired);
}

bool user_handler_was_created_by_us() {
    return read_default_value(HKEY_CURRENT_USER, kUserSchemeKey) ==
           kHandlerDescription;
}

}  // namespace

bool register_nintendo_auth_protocol() {
    if (current_handler_is_ours()) {
        // This can be a handler left behind by a previous crash. Mark it as
        // owned for this run so a successful/cancelled login can clean it up.
        g_created_handler = true;
        return true;
    }

    HKEY existing = nullptr;
    if (RegOpenKeyExW(
            HKEY_CLASSES_ROOT,
            kSchemeKey,
            0,
            KEY_READ,
            &existing) == ERROR_SUCCESS) {
        RegCloseKey(existing);
        if (!user_handler_was_created_by_us()) {
            return false;
        }

        // A previous crash or moving/updating the executable can leave our old
        // command registered. It is safe to replace only our own marked HKCU
        // handler; never take the scheme away from nxapi or another client.
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
    }

    const auto command = desired_command();
    if (command.empty()) {
        return false;
    }

    HKEY scheme = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kUserSchemeKey,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &scheme,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const bool root_ok =
        write_string_value(scheme, nullptr, kHandlerDescription) &&
        write_string_value(scheme, L"URL Protocol", L"");
    RegCloseKey(scheme);
    if (!root_ok) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
        return false;
    }

    HKEY command_key = nullptr;
    const std::wstring command_path =
        std::wstring(kUserSchemeKey) + kCommandSuffix;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            command_path.c_str(),
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &command_key,
            nullptr) != ERROR_SUCCESS) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
        return false;
    }

    const bool command_ok = write_string_value(command_key, nullptr, command);
    RegCloseKey(command_key);
    if (!command_ok) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
        return false;
    }

    g_created_handler = true;
    return true;
}

void unregister_nintendo_auth_protocol() {
    if (!g_created_handler || !current_handler_is_ours()) {
        return;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
    g_created_handler = false;
}

bool forward_nintendo_auth_callback_to_running_instance(const std::string& url) {
    if (!is_nintendo_auth_callback(url)) {
        return false;
    }

    HWND target = FindWindowExW(HWND_MESSAGE, nullptr, kTrayClass, nullptr);
    if (target == nullptr) {
        return false;
    }

    COPYDATASTRUCT data{};
    data.dwData = kAuthCallbackCopyDataId;
    data.cbData = static_cast<DWORD>(url.size() + 1);
    data.lpData = const_cast<char*>(url.c_str());

    DWORD_PTR handled = 0;
    const LRESULT sent = SendMessageTimeoutW(
        target,
        WM_COPYDATA,
        0,
        reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        2000,
        &handled);
    return sent != 0 && handled != 0;
}

}  // namespace nso

#endif  // _WIN32
