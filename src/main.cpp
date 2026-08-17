#include "nso_album_sync/app.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        nso::App app;
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}
