#include "nso_album_sync/platform.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace nso {
namespace {
constexpr UINT kTrayMessage = WM_APP + 42;
constexpr int kIconId = 101;
constexpr int kDisclosureSourceButton = 1101;
constexpr wchar_t kTrayClass[] = L"NSOAlbumSyncTray";
constexpr wchar_t kPromptClass[] = L"NSOAlbumSyncPrompt";
constexpr wchar_t kDisclosureClass[] = L"NSOAlbumSyncDisclosure";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NSO Album Sync";
constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiSourceUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api";

enum Command : UINT {
    CmdSync = 1001, CmdAuto, CmdNotifications, CmdDiscord, CmdFolder,
    CmdOpen, CmdStartup, CmdProxy, CmdAccount, CmdExit,
};

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

HICON app_icon() {
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconId));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

std::wstring executable_path_wide() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring auto_label(int minutes) {
    if (minutes == 60) return L"Auto-Sync (Hourly)";
    return L"Auto-Sync (Every " + std::to_wstring(std::max(1, minutes)) + L" min)";
}

struct PromptState {
    HWND edit = nullptr;
    bool done = false;
    bool accepted = false;
};

struct DisclosureState {
    bool done = false;
    bool accepted = false;
};

LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<PromptState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) { state->accepted = true; state->done = true; return 0; }
        if (LOWORD(wp) == IDCANCEL) { state->done = true; return 0; }
    }
    if (state && msg == WM_CLOSE) { state->done = true; return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK disclosure_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DisclosureState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<DisclosureState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state && msg == WM_COMMAND) {
        const auto command = LOWORD(wp);
        if (command == IDOK) {
            state->accepted = true;
            state->done = true;
            return 0;
        }
        if (command == IDCANCEL) {
            state->done = true;
            return 0;
        }
        if (command == kDisclosureSourceButton) {
            const auto url = wide(kNxapiSourceUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }
    if (state && msg == WM_CLOSE) {
        state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string prompt_box(const std::string& title, const std::string& text,
                       const std::string& initial) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = instance;
    wc.hIcon = app_icon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPromptClass;
    RegisterClassW(&wc);

    const bool is_proxy = title == "HTTP Proxy";
    PromptState state;
    const int width = 680;
    const int height = is_proxy ? 245 : 310;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kPromptClass, wide(title).c_str(), WS_CAPTION | WS_SYSMENU,
        x, y, width, height, nullptr, nullptr, instance, &state);
    if (!window) return {};

    const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto make = [&](const wchar_t* cls, const std::wstring& caption,
                          DWORD style, int cx, int cy, int cw, int ch, int id) {
        HWND control = CreateWindowW(cls, caption.c_str(), WS_CHILD | WS_VISIBLE | style,
            cx, cy, cw, ch, window,
            id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };

    const int message_height = is_proxy ? 48 : 108;
    const int edit_y = is_proxy ? 104 : 168;
    const int button_y = is_proxy ? 154 : 218;
    make(L"STATIC", L"Nintendo Switch Online  ·  Album Sync", 0, 20, 16, 630, 22, 0);
    make(L"STATIC", wide(text), SS_LEFT, 20, 48, 630, message_height, 0);
    state.edit = make(L"EDIT", wide(initial), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        20, edit_y, 630, 28, 10);
    make(
        L"BUTTON",
        is_proxy ? L"Save" : L"Continue",
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        470,
        button_y,
        86,
        30,
        IDOK);
    make(L"BUTTON", L"Cancel", WS_TABSTOP, 564, button_y, 86, 30, IDCANCEL);

    ShowWindow(window, SW_SHOW);
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);
    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == state.edit) {
            if (msg.wParam == VK_RETURN) { state.accepted = true; state.done = true; continue; }
            if (msg.wParam == VK_ESCAPE) { state.done = true; continue; }
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    std::string result;
    if (state.accepted) {
        const int n = GetWindowTextLengthW(state.edit);
        std::wstring value(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(state.edit, value.data(), n + 1);
        value.resize(static_cast<std::size_t>(n));
        result = utf8(value);
    }
    DestroyWindow(window);
    return result;
}

bool disclosure_box(const std::string& title, const std::string& text) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = disclosure_proc;
    wc.hInstance = instance;
    wc.hIcon = app_icon();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kDisclosureClass;
    RegisterClassW(&wc);

    DisclosureState state;
    const int width = 760, height = 430;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kDisclosureClass, wide(title).c_str(), WS_CAPTION | WS_SYSMENU,
        x, y, width, height, nullptr, nullptr, instance, &state);
    if (!window) return false;

    const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto make = [&](const wchar_t* cls, const std::wstring& caption,
                          DWORD style, int cx, int cy, int cw, int ch, int id) {
        HWND control = CreateWindowW(cls, caption.c_str(), WS_CHILD | WS_VISIBLE | style,
            cx, cy, cw, ch, window,
            id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };

    make(L"STATIC", L"Nintendo Switch Online  ·  Album Sync", 0, 20, 16, 700, 22, 0);
    make(L"STATIC", wide(text), SS_LEFT, 20, 50, 700, 275, 0);
    make(
        L"BUTTON",
        L"View Source",
        WS_TABSTOP,
        384,
        340,
        106,
        30,
        kDisclosureSourceButton);
    make(
        L"BUTTON",
        L"Continue",
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        500,
        340,
        106,
        30,
        IDOK);
    make(L"BUTTON", L"Cancel", WS_TABSTOP, 616, 340, 106, 30, IDCANCEL);

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(window);
    return state.accepted;
}
}  // namespace

struct PlatformUi::Impl {
    HWND window = nullptr;
    NOTIFYICONDATAW tray{};
    PlatformCallbacks callbacks;
    MenuState state;
    std::mutex mutex;
};

namespace {
PlatformUi::Impl* g_ui = nullptr;

void invoke(PlatformUi::Impl* ui, UINT command) {
    if (!ui) return;
    const auto& c = ui->callbacks;
    switch (command) {
        case CmdSync: c.sync_now(); break;
        case CmdAuto: c.toggle_auto(); break;
        case CmdNotifications: c.toggle_notifications(); break;
        case CmdDiscord: c.toggle_discord(); break;
        case CmdFolder: c.select_folder(); break;
        case CmdOpen: c.open_folder(); break;
        case CmdStartup: c.toggle_start(); break;
        case CmdProxy: c.proxy(); break;
        case CmdAccount: c.sign_in_out(); break;
        case CmdExit: c.exit(); break;
        default: break;
    }
}

void tray_menu(PlatformUi::Impl* ui) {
    if (!ui) return;
    MenuState s;
    { std::lock_guard lock(ui->mutex); s = ui->state; }
    HMENU menu = CreatePopupMenu();
    const auto add = [&](UINT id, const std::wstring& label, UINT flags = MF_STRING) {
        AppendMenuW(menu, flags, id, label.c_str());
    };
    add(0, s.signed_in ? L"Connected as " + wide(s.nickname) : L"Not signed in", MF_GRAYED);
    add(0, L"Last sync: " + wide(s.last_sync), MF_GRAYED);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdSync, L"Sync Now", s.signed_in ? MF_STRING : MF_GRAYED);
    add(CmdAuto, auto_label(s.sync_interval_minutes), MF_STRING | (s.auto_sync ? MF_CHECKED : 0));
    add(CmdNotifications, L"Notifications", MF_STRING | (s.notifications ? MF_CHECKED : 0));
    add(CmdDiscord, L"Discord Rich Presence", MF_STRING | (s.discord ? MF_CHECKED : 0));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdFolder, L"Choose Album Folder…");
    add(CmdOpen, L"Open Album Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdStartup, L"Start on Boot", MF_STRING | (s.start_on_boot ? MF_CHECKED : 0));
    add(CmdProxy, L"HTTP Proxy…");
    add(CmdAccount, s.signed_in ? L"Sign Out" : L"Sign In to Nintendo Account…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(CmdExit, L"Exit");
    POINT p{}; GetCursorPos(&p); SetForegroundWindow(ui->window);
    const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
        p.x, p.y, 0, ui->window, nullptr);
    DestroyMenu(menu);
    invoke(ui, selected);
}

LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayMessage) {
        const UINT event = LOWORD(lp);
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) { tray_menu(g_ui); return 0; }
        if (event == WM_LBUTTONDBLCLK) { invoke(g_ui, CmdOpen); return 0; }
    }
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

PlatformUi::PlatformUi() : impl_(new Impl) {}
PlatformUi::~PlatformUi() { delete impl_; }

void PlatformUi::run(const PlatformCallbacks& callbacks) {
    impl_->callbacks = callbacks; g_ui = impl_;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = tray_proc; wc.hInstance = instance;
    wc.hIcon = app_icon(); wc.lpszClassName = kTrayClass; RegisterClassW(&wc);
    impl_->window = CreateWindowExW(0, kTrayClass, L"NSO Album Sync", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    impl_->tray.cbSize = sizeof(impl_->tray); impl_->tray.hWnd = impl_->window;
    impl_->tray.uID = 1; impl_->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    impl_->tray.uCallbackMessage = kTrayMessage; impl_->tray.hIcon = app_icon();
    wcscpy_s(impl_->tray.szTip, L"NSO Album Sync");
    if (!Shell_NotifyIconW(NIM_ADD, &impl_->tray)) {
        MessageBoxW(nullptr, L"Could not create the NSO Album Sync tray icon.",
            L"NSO Album Sync", MB_OK | MB_ICONERROR); return;
    }
    impl_->tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &impl_->tray);
    if (callbacks.ready) callbacks.ready();
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Shell_NotifyIconW(NIM_DELETE, &impl_->tray);
}

void PlatformUi::stop() { if (impl_->window) PostMessageW(impl_->window, WM_CLOSE, 0, 0); }
void PlatformUi::update(const MenuState& state) { std::lock_guard lock(impl_->mutex); impl_->state = state; }

void PlatformUi::notify(const std::string& title, const std::string& message) {
    if (!impl_->window) return;
    std::lock_guard lock(impl_->mutex);
    impl_->tray.uFlags = NIF_INFO;
    wcsncpy_s(impl_->tray.szInfo, wide(message).c_str(), _TRUNCATE);
    wcsncpy_s(impl_->tray.szInfoTitle, wide(title).c_str(), _TRUNCATE);
    impl_->tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &impl_->tray);
    impl_->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

std::string PlatformUi::prompt(const std::string& title, const std::string& message,
                               const std::string& initial) {
    return prompt_box(title, message, initial);
}

bool PlatformUi::confirm(const std::string& title, const std::string& message) {
    if (title == kNxapiDisclosureTitle) {
        return disclosure_box(title, message);
    }
    return MessageBoxW(impl_->window, wide(message).c_str(), wide(title).c_str(),
        MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES;
}

std::string PlatformUi::choose_folder(const std::string& initial) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    std::string selected;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog))) && dialog) {
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose Nintendo Switch Album Folder");
        if (!initial.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(wide(initial).c_str(), nullptr,
                                                     IID_PPV_ARGS(&folder))) && folder) {
                dialog->SetFolder(folder); folder->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(impl_->window))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    selected = utf8(path); CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return selected;
}

bool start_on_boot_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key); return exists;
}

void set_start_on_boot(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (!enabled) { RegDeleteValueW(key, kRunValue); RegCloseKey(key); return; }
    const auto exe = executable_path_wide();
    if (exe.empty()) { RegCloseKey(key); return; }
    const std::wstring value = L"\"" + exe + L"\"";
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}
}  // namespace nso
#endif
