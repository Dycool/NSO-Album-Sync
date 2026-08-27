#include "nso_album_sync/secure_store.hpp"

#ifdef __APPLE__

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace nso {
namespace {

std::filesystem::path credentials_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    const auto dir = std::filesystem::path(home) / "Library" / "Application Support" / "NSOAlbumSync" / "credentials";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
        chmod(dir.c_str(), 0700);
    }
    return dir;
}

std::filesystem::path credential_file_path(const std::string& account) {
    const auto dir = credentials_directory();
    if (dir.empty()) return {};

    // Sanitize account name for safe filename
    std::string safe_name;
    for (char c : account) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            safe_name += c;
        } else {
            safe_name += '_';
        }
    }
    if (safe_name.empty()) safe_name = "default";
    return dir / (safe_name + ".dat");
}

}  // namespace

bool SecureStore::available() {
    return !credentials_directory().empty();
}

bool SecureStore::put(
    const std::string& account,
    const std::string& secret) {
    const auto file_path = credential_file_path(account);
    if (file_path.empty()) return false;

    std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;

    file.write(secret.data(), static_cast<std::streamsize>(secret.size()));
    file.close();

    chmod(file_path.c_str(), 0600);
    return true;
}

std::optional<std::string> SecureStore::get(const std::string& account) {
    const auto file_path = credential_file_path(account);
    if (file_path.empty() || !std::filesystem::exists(file_path)) {
        return std::nullopt;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::string content(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (content.empty()) return std::nullopt;
    return content;
}

void SecureStore::erase(const std::string& account) {
    const auto file_path = credential_file_path(account);
    if (!file_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(file_path, ec);
    }
}

}  // namespace nso

#endif  // __APPLE__
