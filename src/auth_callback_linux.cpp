#include "nso_album_sync/auth_callback.hpp"

#if defined(__linux__)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace nso {
namespace {

constexpr char kDesktopId[] = "nso-album-sync-auth.desktop";
constexpr char kMimeType[] = "x-scheme-handler/npf71b963c1b7b6d119";

std::filesystem::path desktop_file() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::filesystem::path(home) / ".local" / "share" / "applications" / kDesktopId;
}

std::string executable_path() {
    const char* appimage = std::getenv("APPIMAGE");
    if (appimage != nullptr && *appimage != '\0') return appimage;
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::string desktop_quote(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"' || ch == '`' || ch == '$') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return "\"" + escaped + "\"";
}

std::pair<bool, std::string> query_default_handler() {
    const std::string command = std::string("xdg-mime query default ") + kMimeType + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return {false, {}};

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();
    const int status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return {false, {}};

    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    return {true, output};
}

}  // namespace

bool register_nintendo_auth_protocol() {
    const auto [query_ok, current] = query_default_handler();
    if (!query_ok) return false;
    if (!current.empty() && current != kDesktopId) return false;

    const auto file = desktop_file();
    const auto executable = executable_path();
    if (file.empty() || executable.empty()) return false;

    try {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::trunc);
        if (!output) return false;
        output
            << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Name=NSO Album Sync Nintendo Account Sign-In\n"
            << "Exec=" << desktop_quote(executable) << " %u\n"
            << "NoDisplay=true\n"
            << "Terminal=false\n"
            << "MimeType=" << kMimeType << ";\n";
        output.close();
        if (!output) return false;
    } catch (...) {
        return false;
    }

    if (current == kDesktopId) return true;
    const std::string command = std::string("xdg-mime default ") + kDesktopId + " " + kMimeType + " >/dev/null 2>&1";
    const int status = std::system(command.c_str());
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void unregister_nintendo_auth_protocol() {
}

}  // namespace nso

#endif  // __linux__
