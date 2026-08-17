#include "nso_album_sync/platform.hpp"

#ifdef _WIN32

#include "nso_album_sync/windows_compat.hpp"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

namespace nso {
namespace {

constexpr UINT kTrayMessage = WM_APP + 42;
constexpr int kAppIconResourceId = 101;
constexpr wchar_t kTrayWindowClass[] = L"NsoAlbumSyncTray";
constexpr wchar_t kRunRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunRegistryValue[] = L"NSO Album Sync";

enum MenuCommand : UINT {
    SyncNow = 1001,
    ToggleAutoSync,
    ToggleNotifications,
    ToggleDiscord,
    SelectFolder,
    OpenFolder,
    ToggleStartOnBoot,
    ConfigureProxy,
    SignInOrOut,
    Exit,
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int input_length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        input_length,
        nullptr,
        0);

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        input_length,
        result.data(),
        required);

    return result;
}

HICON load_app_icon() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON icon = LoadIconW(
        instance,
        MAKEINTRESOURCEW(kAppIconResourceId));
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        result.data(),
        required,
        nullptr,
        nullptr);

    return result;
}

}  // namespace

struct PlatformUi::Impl {
    HWND window = nullptr;
    NOTIFYICONDATAW tray_icon{};
    PlatformCallbacks callbacks;
    MenuState state;
};

namespace {

PlatformUi::Impl* g_platform = nullptr;

void dispatch_command(PlatformUi::Impl* impl, UINT command) {
    if (impl == nullptr) {
        return;
    }

    const auto& callbacks = impl->callbacks;
    switch (command) {
        case SyncNow:
            callbacks.sync_now();
            break;
        case ToggleAutoSync:
            callbacks.toggle_auto();
            break;
        case ToggleNotifications:
            callbacks.toggle_notifications();
            break;
        case ToggleDiscord:
            callbacks.toggle_discord();
            break;
        case SelectFolder:
            callbacks.select_folder();
            break;
        case OpenFolder:
            callbacks.open_folder();
            break;
        case ToggleStartOnBoot:
            callbacks.toggle_start();
            break;
        case ConfigureProxy:
            callbacks.proxy();
            break;
        case SignInOrOut:
            callbacks.sign_in_out();
            break;
        case Exit:
            callbacks.exit();
            break;
        default:
            break;
    }
}

void show_tray_menu(PlatformUi::Impl* impl) {
    HMENU menu = CreatePopupMenu();

    const auto append_item = [&](UINT id,
                                 const std::wstring& label,
                                 UINT flags = MF_STRING) {
        AppendMenuW(menu, flags, id, label.c_str());
    };

    const auto& state = impl->state;
    const auto account_label = state.signed_in
        ? L"Connected (" + utf8_to_wide(state.nickname) + L")"
        : L"Not signed in";

    append_item(0, account_label, MF_STRING | MF_GRAYED);
    append_item(
        0,
        L"Last sync: " + utf8_to_wide(state.last_sync),
        MF_STRING | MF_GRAYED);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_item(SyncNow, L"Sync Now");
    append_item(
        ToggleAutoSync,
        L"Auto-Sync (Hourly)",
        MF_STRING | (state.auto_sync ? MF_CHECKED : 0));
    append_item(
        ToggleNotifications,
        L"Notifications",
        MF_STRING | (state.notifications ? MF_CHECKED : 0));
    append_item(
        ToggleDiscord,
        L"Discord Rich Presence",
        MF_STRING | (state.discord ? MF_CHECKED : 0));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_item(SelectFolder, L"Select Folder...");
    append_item(OpenFolder, L"Open Album Folder");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_item(
        ToggleStartOnBoot,
        L"Start on Boot",
        MF_STRING | (state.start_on_boot ? MF_CHECKED : 0));
    append_item(ConfigureProxy, L"HTTP Proxy...");
    append_item(SignInOrOut, state.signed_in ? L"Sign Out" : L"Sign In");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_item(Exit, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(impl->window);

    const UINT selected = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        impl->window,
        nullptr);

    DestroyMenu(menu);
    dispatch_command(impl, selected);
}

LRESULT CALLBACK tray_window_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    (void)w_param;

    if (message == kTrayMessage && l_param == WM_RBUTTONUP) {
        show_tray_menu(g_platform);
        return 0;
    }

    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

std::string input_box(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kInputWindowClass[] = L"NsoInputBox";

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.hIcon = load_app_icon();
    window_class.lpszClassName = kInputWindowClass;
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kInputWindowClass,
        utf8_to_wide(title).c_str(),
        WS_CAPTION | WS_SYSMENU,
        400,
        300,
        520,
        180,
        nullptr,
        nullptr,
        instance,
        nullptr);

    CreateWindowW(
        L"STATIC",
        utf8_to_wide(message).c_str(),
        WS_CHILD | WS_VISIBLE,
        15,
        15,
        480,
        40,
        window,
        nullptr,
        instance,
        nullptr);

    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        utf8_to_wide(initial).c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        15,
        60,
        480,
        25,
        window,
        reinterpret_cast<HMENU>(10),
        instance,
        nullptr);

    HWND ok_button = CreateWindowW(
        L"BUTTON",
        L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        325,
        100,
        80,
        28,
        window,
        reinterpret_cast<HMENU>(IDOK),
        instance,
        nullptr);

    CreateWindowW(
        L"BUTTON",
        L"Cancel",
        WS_CHILD | WS_VISIBLE,
        415,
        100,
        80,
        28,
        window,
        reinterpret_cast<HMENU>(IDCANCEL),
        instance,
        nullptr);

    ShowWindow(window, SW_SHOW);

    bool done = false;
    bool accepted = false;
    MSG message_record{};

    while (!done && GetMessageW(&message_record, nullptr, 0, 0) > 0) {
        if (message_record.hwnd == ok_button &&
            message_record.message == WM_LBUTTONUP) {
            accepted = true;
            done = true;
        } else if (message_record.message == WM_COMMAND &&
                   LOWORD(message_record.wParam) == IDOK) {
            accepted = true;
            done = true;
        } else if (message_record.message == WM_COMMAND &&
                   LOWORD(message_record.wParam) == IDCANCEL) {
            done = true;
        } else {
            TranslateMessage(&message_record);
            DispatchMessageW(&message_record);
        }
    }

    std::string result;
    if (accepted) {
        const int length = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(edit, text.data(), length + 1);
        text.resize(static_cast<std::size_t>(length));
        result = wide_to_utf8(text);
    }

    DestroyWindow(window);
    return result;
}

}  // namespace

PlatformUi::PlatformUi() : impl_(new Impl) {}

PlatformUi::~PlatformUi() {
    delete impl_;
}

void PlatformUi::run(const PlatformCallbacks& callbacks) {
    impl_->callbacks = callbacks;
    g_platform = impl_;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON app_icon = load_app_icon();

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = tray_window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = app_icon;
    window_class.lpszClassName = kTrayWindowClass;
    RegisterClassW(&window_class);

    impl_->window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"NSO Album Sync",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        window_class.hInstance,
        nullptr);

    impl_->tray_icon.cbSize = sizeof(impl_->tray_icon);
    impl_->tray_icon.hWnd = impl_->window;
    impl_->tray_icon.uID = 1;
    impl_->tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    impl_->tray_icon.uCallbackMessage = kTrayMessage;
    impl_->tray_icon.hIcon = app_icon;
    wcscpy_s(impl_->tray_icon.szTip, L"NSO Album Sync");

    Shell_NotifyIconW(NIM_ADD, &impl_->tray_icon);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    Shell_NotifyIconW(NIM_DELETE, &impl_->tray_icon);
}

void PlatformUi::stop() {
    if (impl_->window != nullptr) {
        PostMessageW(impl_->window, WM_CLOSE, 0, 0);
    }
}

void PlatformUi::update(const MenuState& state) {
    impl_->state = state;
}

void PlatformUi::notify(
    const std::string& title,
    const std::string& message) {
    if (impl_->window == nullptr) {
        return;
    }

    impl_->tray_icon.uFlags = NIF_INFO;
    wcsncpy_s(
        impl_->tray_icon.szInfo,
        utf8_to_wide(message).c_str(),
        _TRUNCATE);
    wcsncpy_s(
        impl_->tray_icon.szInfoTitle,
        utf8_to_wide(title).c_str(),
        _TRUNCATE);
    impl_->tray_icon.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_MODIFY, &impl_->tray_icon);
    impl_->tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

std::string PlatformUi::prompt(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
    return input_box(title, message, initial);
}

bool PlatformUi::confirm(
    const std::string& title,
    const std::string& message) {
    return MessageBoxW(
               nullptr,
               utf8_to_wide(message).c_str(),
               utf8_to_wide(title).c_str(),
               MB_YESNO | MB_ICONQUESTION) == IDYES;
}

std::string PlatformUi::choose_folder(const std::string& initial) {
    (void)initial;

    BROWSEINFOW browse{};
    browse.lpszTitle = L"Select Album Folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) {
        return {};
    }

    wchar_t path[MAX_PATH];
    std::string result;
    if (SHGetPathFromIDListW(item, path)) {
        result = wide_to_utf8(path);
    }

    CoTaskMemFree(item);
    return result;
}

bool start_on_boot_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kRunRegistryPath,
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[2048];
    DWORD size = sizeof(value);
    DWORD type = 0;

    const bool exists = RegQueryValueExW(
        key,
        kRunRegistryValue,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(value),
        &size) == ERROR_SUCCESS;

    RegCloseKey(key);
    return exists;
}

void set_start_on_boot(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunRegistryPath,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (!enabled) {
        RegDeleteValueW(key, kRunRegistryValue);
        RegCloseKey(key);
        return;
    }

    wchar_t executable[MAX_PATH];
    GetModuleFileNameW(nullptr, executable, MAX_PATH);

    const std::wstring quoted = L"\"" + std::wstring(executable) + L"\"";
    RegSetValueExW(
        key,
        kRunRegistryValue,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(quoted.c_str()),
        static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(key);
}

}  // namespace nso

#endif  // _WIN32
