#include "nso_album_sync/auth_callback.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nso {
namespace {

std::filesystem::path callback_path() {
    std::string filename = "nso-album-sync-auth-callback";
#ifndef _WIN32
    filename += "-" + std::to_string(getuid());
#endif
    filename += ".txt";
    return std::filesystem::temp_directory_path() / filename;
}

std::string strip_space(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

bool is_nintendo_auth_callback(const std::string& url) {
    const std::string prefix = kNintendoAuthCallbackPrefix;
    if (url.rfind(prefix, 0) != 0) return false;
    if (url.size() == prefix.size()) return true;
    const char next = url[prefix.size()];
    return next == '#' || next == '?';
}

bool publish_nintendo_auth_callback(const std::string& url) {
    if (!is_nintendo_auth_callback(url)) return false;
    try {
        const auto target = callback_path();
        auto temporary = target;
        temporary += ".tmp";
        std::error_code error;
        std::filesystem::remove(temporary, error);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output << url;
            output.flush();
            if (!output) return false;
        }
#ifndef _WIN32
        chmod(temporary.c_str(), 0600);
#endif
        error.clear();
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> take_nintendo_auth_callback() {
    try {
        const auto target = callback_path();
        std::ifstream input(target, std::ios::binary);
        if (!input) return std::nullopt;
        std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        input.close();
        std::error_code error;
        std::filesystem::remove(target, error);
        value = strip_space(std::move(value));
        if (!is_nintendo_auth_callback(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

void clear_nintendo_auth_callback() {
    try {
        const auto target = callback_path();
        auto temporary = target;
        temporary += ".tmp";
        std::error_code error;
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::remove(temporary, error);
    } catch (...) {
    }
}

}  // namespace nso
