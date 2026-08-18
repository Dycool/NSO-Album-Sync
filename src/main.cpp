#include "nso_album_sync/app.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
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
        handle_ = CreateMutexW(
            nullptr,
            FALSE,
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
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            if (acquired_) {
                flock(fd_, LOCK_UN);
            }
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

int run_application() {
    try {
        SingleInstanceGuard single_instance;
        if (!single_instance.acquired()) {
#ifdef _WIN32
            MessageBoxW(
                nullptr,
                L"NSO Album Sync is already running in the notification area.",
                L"NSO Album Sync",
                MB_OK | MB_ICONINFORMATION);
#else
            std::cerr << "NSO Album Sync is already running.\n";
#endif
            return 0;
        }

        nso::App app;
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

#ifdef _WIN32

// CMake's WIN32 executable mode selects the Windows GUI subsystem so the tray
// application starts without opening a console window. MSVC therefore expects
// a WinMain entry point rather than the standard main function.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return run_application();
}

#else

int main() {
    return run_application();
}

#endif
