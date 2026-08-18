#include "nso_album_sync/app.hpp"
#include "nso_album_sync/auth_callback.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#include <shellapi.h>
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
            if (!auth_callback.empty() &&
                nso::publish_nintendo_auth_callback(auth_callback)) {
                return 0;
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

std::string auth_callback_from_command_line() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return {};

    std::string callback;
    for (int i = 1; i < argc; ++i) {
        const auto value = wide_to_utf8(argv[i]);
        if (nso::is_nintendo_auth_callback(value)) {
            callback = value;
            break;
        }
    }
    LocalFree(argv);
    return callback;
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
