#include "nso_album_sync/auth_callback.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nso {
namespace {

constexpr std::size_t kMaxCallbackBytes = 16 * 1024;
std::atomic<std::uint64_t> g_callback_temp_counter{0};

std::string environment_variable(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::filesystem::path runtime_directory_candidate() {
#ifdef _WIN32
    const auto local_app_data = environment_variable("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return std::filesystem::path(local_app_data) / "NSOAlbumSync" / "Runtime";
    }
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path{} : temporary / "NSOAlbumSync" / "Runtime";
#elif __APPLE__
    const auto home = environment_variable("HOME");
    if (home.empty()) return {};
    return std::filesystem::path(home) /
           "Library" / "Caches" / "NSOAlbumSync" / "Runtime";
#else
    const auto xdg_runtime = environment_variable("XDG_RUNTIME_DIR");
    if (!xdg_runtime.empty()) {
        return std::filesystem::path(xdg_runtime) / "NSOAlbumSync";
    }
    const auto home = environment_variable("HOME");
    if (home.empty()) return {};
    return std::filesystem::path(home) /
           ".cache" / "NSOAlbumSync" / "runtime";
#endif
}

bool ensure_private_runtime_directory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

#ifdef _WIN32
    return std::filesystem::is_directory(directory, error) && !error;
#else
    struct stat information{};
    if (lstat(directory.c_str(), &information) != 0 ||
        !S_ISDIR(information.st_mode) ||
        information.st_uid != getuid()) {
        return false;
    }
    if (chmod(directory.c_str(), 0700) != 0) return false;
    return true;
#endif
}

std::filesystem::path callback_path() {
    const auto directory = application_runtime_directory();
    return directory.empty() ? std::filesystem::path{} : directory / "auth-callback.txt";
}

std::filesystem::path callback_temporary_path(
    const std::filesystem::path& target) {
    const auto sequence = g_callback_temp_counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const auto process = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto process = static_cast<unsigned long>(getpid());
#endif
    return target.parent_path() /
           ("auth-callback.tmp." + std::to_string(process) + "." +
            std::to_string(sequence));
}

std::string strip_space(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ascii_equal_case_insensitive(
    std::string_view left,
    std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto l = static_cast<unsigned char>(left[i]);
        const auto r = static_cast<unsigned char>(right[i]);
        if (std::tolower(l) != std::tolower(r)) return false;
    }
    return true;
}

#ifndef _WIN32
bool write_all(int descriptor, const char* data, std::size_t size) {
    while (size > 0) {
        const auto written = write(descriptor, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool publish_posix_callback(
    const std::filesystem::path& target,
    const std::string& url) {
    if (url.size() > kMaxCallbackBytes) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto temporary = callback_temporary_path(target);
        const int descriptor = open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            return false;
        }

        const bool wrote = write_all(descriptor, url.data(), url.size());
        const bool synced = wrote && fsync(descriptor) == 0;
        const bool closed = close(descriptor) == 0;
        if (!wrote || !synced || !closed) {
            unlink(temporary.c_str());
            return false;
        }

        if (rename(temporary.c_str(), target.c_str()) != 0) {
            unlink(temporary.c_str());
            return false;
        }
        return true;
    }
    return false;
}

std::optional<std::string> take_posix_callback(
    const std::filesystem::path& target) {
    const int descriptor = open(target.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return std::nullopt;

    struct stat information{};
    if (fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) ||
        information.st_uid != getuid() ||
        (information.st_mode & 0077) != 0 ||
        information.st_size < 0 ||
        static_cast<std::uint64_t>(information.st_size) > kMaxCallbackBytes) {
        close(descriptor);
        unlink(target.c_str());
        return std::nullopt;
    }

    std::string value(static_cast<std::size_t>(information.st_size), '\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto count = read(
            descriptor,
            value.data() + offset,
            value.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        close(descriptor);
        unlink(target.c_str());
        return std::nullopt;
    }

    close(descriptor);
    unlink(target.c_str());
    return value;
}
#endif

}  // namespace

std::filesystem::path application_runtime_directory() {
    const auto directory = runtime_directory_candidate();
    return ensure_private_runtime_directory(directory)
        ? directory
        : std::filesystem::path{};
}

bool is_nintendo_auth_callback(const std::string& url) {
    const std::string_view prefix = kNintendoAuthCallbackPrefix;
    if (url.size() < prefix.size() ||
        !ascii_equal_case_insensitive(
            std::string_view(url).substr(0, prefix.size()), prefix)) {
        return false;
    }

    std::size_t position = prefix.size();
    if (position == url.size()) return true;

    // Chromium/Windows can canonicalize a custom URI whose authority is
    // "auth" by inserting the otherwise-empty path slash. Nintendo's
    // npf...://auth#... callback can therefore arrive as npf...://auth/#...
    // (or npf...://auth/?...). Treat those forms as the same callback.
    if (url[position] == '/') {
        ++position;
        if (position == url.size()) return true;
    }

    return url[position] == '#' || url[position] == '?';
}

bool publish_nintendo_auth_callback(const std::string& url) {
    if (!is_nintendo_auth_callback(url) || url.size() > kMaxCallbackBytes) {
        return false;
    }

    try {
        const auto target = callback_path();
        if (target.empty()) return false;

#ifdef _WIN32
        const auto temporary = callback_temporary_path(target);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output << url;
            output.flush();
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        std::error_code error;
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
#else
        return publish_posix_callback(target, url);
#endif
    } catch (...) {
        return false;
    }
}

std::optional<std::string> take_nintendo_auth_callback() {
    try {
        const auto target = callback_path();
        if (target.empty()) return std::nullopt;

        std::string value;
#ifdef _WIN32
        std::ifstream input(target, std::ios::binary);
        if (!input) return std::nullopt;
        value.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        input.close();
        std::error_code error;
        std::filesystem::remove(target, error);
        if (value.size() > kMaxCallbackBytes) return std::nullopt;
#else
        const auto taken = take_posix_callback(target);
        if (!taken) return std::nullopt;
        value = *taken;
#endif

        value = strip_space(std::move(value));
        if (!is_nintendo_auth_callback(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

void clear_nintendo_auth_callback() {
    try {
        const auto directory = application_runtime_directory();
        if (directory.empty()) return;

        std::error_code error;
        std::filesystem::remove(directory / "auth-callback.txt", error);

        // Clean only our own stale temporary callback files from the private
        // runtime directory. They contain no long-lived Nintendo credential,
        // but should not survive a crash longer than necessary.
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory,
                 std::filesystem::directory_options::skip_permission_denied,
                 error)) {
            if (error) break;
            const auto name = entry.path().filename().string();
            if (name.rfind("auth-callback.tmp.", 0) == 0) {
                std::filesystem::remove(entry.path(), error);
                error.clear();
            }
        }
    } catch (...) {
    }
}

}  // namespace nso
