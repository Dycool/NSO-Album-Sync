#include "nso_album_sync/app.hpp"

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#endif

#include <exception>
#include <iostream>

namespace {

int run_application() {
    try {
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
// application starts without opening a console window.  MSVC therefore expects
// a WinMain entry point rather than the standard main function.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return run_application();
}

#else

int main() {
    return run_application();
}

#endif
