#include "nso_album_sync/app.hpp"
#include "nso_album_sync/auth_callback.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#include <shellapi.h>
#ifdef _MSC_VER
// Let MSVC/CMake generate the executable manifest and merge this dependency
// instead of embedding a second RT_MANIFEST resource through app.rc.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

class SingleInstanceGuard {
public:
    SingleInstanceGuard() {
#ifdef _WIN32
        handle_ = CreateMutexW(nullptr, FALSE,
            L"Local\\NSOAlbumSync_SingleInstance_Mutex_f8bb0128");
        acquired_ = handle_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
#else
        const auto path = std::filesystem::temp_directory_path() /
            ("nso-album-sync-" + std::to_string(getuid()) + ".lock");
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        acquired_ = fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0;
#endif
    }

    ~SingleInstanceGuard() {
#ifdef _WIN32
        if (handle_ != nullptr) CloseHandle(handle_);
#else
        if (fd_ >= 0) {
            if (acquired_) flock(fd_, LOCK_UN);
            close(fd_);
        }
#endif
    }

    bool acquired() const { return acquired_; }

private:
    bool acquired_ = false;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

int run_application(const std::string& auth_callback) {
    try {
        SingleInstanceGuard single_instance;
        if (!single_instance.acquired()) {
            if (!auth_callback.empty()) {
                // Protocol callbacks launch a second copy of the executable.
                // Forward that URL to the already-running tray process instead
                // of treating the launch as an ordinary second app instance.
                for (int attempt = 0; attempt < 5; ++attempt) {
                    if (nso::publish_nintendo_auth_callback(auth_callback)) {
                        return 0;
                    }
#ifdef _WIN32
                    Sleep(20);
#endif
                }
            }
#ifdef _WIN32
            MessageBoxW(nullptr,
                L"NSO Album Sync is already running in the notification area.",
                L"NSO Album Sync", MB_OK | MB_ICONINFORMATION);
#else
            std::cerr << "NSO Album Sync is already running.\n";
#endif
            return 0;
        }

        if (!auth_callback.empty()) {
            nso::publish_nintendo_auth_callback(auth_callback);
        }

        nso::App app;
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::string extract_auth_callback(const std::string& text) {
    const std::string prefix = nso::kNintendoAuthCallbackPrefix;
    const auto start = text.find(prefix);
    if (start == std::string::npos) return {};

    auto end = text.find_first_of("\"' \t\r\n", start);
    if (end == std::string::npos) end = text.size();
    const auto callback = text.substr(start, end - start);
    return nso::is_nintendo_auth_callback(callback) ? callback : std::string{};
}

std::string auth_callback_from_command_line() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            if (const auto callback = extract_auth_callback(wide_to_utf8(argv[i]));
                !callback.empty()) {
                LocalFree(argv);
                return callback;
            }
        }
        LocalFree(argv);
    }

    // Some browser/shell protocol launches do not survive argv tokenisation in
    // exactly the form we registered. Scan the raw command line as a fallback.
    return extract_auth_callback(wide_to_utf8(GetCommandLineW()));
}
#endif

}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return run_application(auth_callback_from_command_line());
}
#else
int main(int argc, char* argv[]) {
    std::string callback;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && nso::is_nintendo_auth_callback(argv[i])) {
            callback = argv[i];
            break;
        }
    }
    return run_application(callback);
}
#endif
