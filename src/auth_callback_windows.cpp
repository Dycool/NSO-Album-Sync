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

bool g_created_handler = false;

struct ExistingUserHandler {
    bool present = false;
    struct Value {
        std::wstring name;
        DWORD type = 0;
        std::vector<BYTE> data;
    };
    struct Key {
        std::wstring path;
        std::vector<Value> values;
    };
    std::vector<Key> keys;
};

ExistingUserHandler g_previous_handler;

bool snapshot_registry_key(
    HKEY root,
    const std::wstring& path,
    const std::wstring& relative_path,
    ExistingUserHandler& snapshot) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value_count = 0;
    DWORD max_value_name = 0;
    DWORD max_value_data = 0;
    DWORD subkey_count = 0;
    DWORD max_subkey_name = 0;
    RegQueryInfoKeyW(
        key, nullptr, nullptr, nullptr, &subkey_count, &max_subkey_name,
        nullptr, &value_count, &max_value_name, &max_value_data, nullptr,
        nullptr);

    ExistingUserHandler::Key saved_key;
    saved_key.path = relative_path;
    std::vector<wchar_t> value_name(max_value_name + 1);
    std::vector<BYTE> value_data(max_value_data == 0 ? 1 : max_value_data);
    for (DWORD i = 0; i < value_count; ++i) {
        DWORD name_length = static_cast<DWORD>(value_name.size());
        DWORD data_size = static_cast<DWORD>(value_data.size());
        DWORD type = 0;
        if (RegEnumValueW(
                key, i, value_name.data(), &name_length, nullptr, &type,
                value_data.data(), &data_size) == ERROR_SUCCESS) {
            ExistingUserHandler::Value value;
            value.name.assign(value_name.data(), name_length);
            value.type = type;
            value.data.assign(value_data.begin(), value_data.begin() + data_size);
            saved_key.values.push_back(std::move(value));
        }
    }
    snapshot.keys.push_back(std::move(saved_key));

    std::vector<wchar_t> subkey_name(max_subkey_name + 1);
    for (DWORD i = 0; i < subkey_count; ++i) {
        DWORD name_length = static_cast<DWORD>(subkey_name.size());
        if (RegEnumKeyExW(
                key, i, subkey_name.data(), &name_length, nullptr, nullptr,
                nullptr, nullptr) == ERROR_SUCCESS) {
            const std::wstring child_name(subkey_name.data(), name_length);
            const auto child_path = path + L"\\" + child_name;
            const auto child_relative = relative_path.empty()
                ? child_name : relative_path + L"\\" + child_name;
            snapshot_registry_key(root, child_path, child_relative, snapshot);
        }
    }
    RegCloseKey(key);
    return true;
}

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

ExistingUserHandler save_existing_user_handler() {
    ExistingUserHandler saved;
    if (!snapshot_registry_key(
            HKEY_CURRENT_USER, kUserSchemeKey, L"", saved)) {
        return saved;
    }
    saved.present = true;
    return saved;
}

void restore_existing_user_handler() {
    if (!g_previous_handler.present) return;

    for (const auto& saved_key : g_previous_handler.keys) {
        const auto path = saved_key.path.empty()
            ? std::wstring(kUserSchemeKey)
            : std::wstring(kUserSchemeKey) + L"\\" + saved_key.path;
        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE,
                nullptr, &key, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        for (const auto& value : saved_key.values) {
            RegSetValueExW(
                key, value.name.empty() ? nullptr : value.name.c_str(), 0,
                value.type, value.data.empty() ? nullptr : value.data.data(),
                static_cast<DWORD>(value.data.size()));
        }
        RegCloseKey(key);
    }
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
        // Preserve any per-user handler so the temporary login override can
        // be removed without stealing the scheme from another application.
        if (!current_handler_is_ours()) {
            g_previous_handler = save_existing_user_handler();
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
    }

    if (!g_previous_handler.present) {
        g_previous_handler = save_existing_user_handler();
    }
    const auto command = desired_command();
    if (command.empty()) {
        // No override was successfully installed; put back anything we
        // removed while preparing the temporary registration.
        restore_existing_user_handler();
        g_previous_handler = {};
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
        restore_existing_user_handler();
        g_previous_handler = {};
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
        restore_existing_user_handler();
        g_previous_handler = {};
        return false;
    }

    const bool command_ok = write_string_value(command_key, nullptr, command);
    RegCloseKey(command_key);
    if (!command_ok) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kUserSchemeKey);
        restore_existing_user_handler();
        g_previous_handler = {};
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
    restore_existing_user_handler();
    g_previous_handler = {};
    g_created_handler = false;
}

}  // namespace nso

#endif  // _WIN32
