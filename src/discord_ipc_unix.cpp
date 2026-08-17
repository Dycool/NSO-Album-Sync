#include "nso_album_sync/discord_ipc.hpp"

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr long kIoTimeoutSeconds = 2;

std::vector<std::filesystem::path> candidate_directories() {
    std::vector<std::filesystem::path> directories;

    for (const char* variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            const std::filesystem::path directory(value);
            if (std::find(directories.begin(), directories.end(), directory) ==
                directories.end()) {
                directories.push_back(directory);
            }
        }
    }

    const std::filesystem::path fallback("/tmp");
    if (std::find(directories.begin(), directories.end(), fallback) ==
        directories.end()) {
        directories.push_back(fallback);
    }

    return directories;
}

class UnixDiscordIpcTransport final : public DiscordIpcTransport {
public:
    ~UnixDiscordIpcTransport() override {
        close();
    }

    bool connect() override {
        close();

        for (const auto& directory : candidate_directories()) {
            for (int index = 0; index < 10; ++index) {
                const auto path =
                    (directory / ("discord-ipc-" + std::to_string(index))).string();

                sockaddr_un address{};
                if (path.size() >= sizeof(address.sun_path)) {
                    continue;
                }

                const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (socket_fd < 0) {
                    continue;
                }

                configure_socket(socket_fd);

                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

                socklen_t address_size = sizeof(address);
#ifdef __APPLE__
                address.sun_len = static_cast<unsigned char>(SUN_LEN(&address));
                address_size = address.sun_len;
#endif

                if (::connect(
                        socket_fd,
                        reinterpret_cast<sockaddr*>(&address),
                        address_size) == 0) {
                    socket_ = socket_fd;
                    return true;
                }

                ::close(socket_fd);
            }
        }

        return false;
    }

    void close() override {
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
    }

    bool connected() const override {
        return socket_ >= 0;
    }

    bool write_frame(const std::vector<std::uint8_t>& frame) override {
        if (!connected() || frame.empty()) {
            return false;
        }

        std::size_t offset = 0;
        while (offset < frame.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const ssize_t written = ::send(
                socket_,
                frame.data() + offset,
                frame.size() - offset,
                flags);

            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }

            if (written < 0 && errno == EINTR) {
                continue;
            }

            close();
            return false;
        }

        return true;
    }

    bool read_exact(void* data, std::size_t size) override {
        auto* output = static_cast<std::uint8_t*>(data);
        std::size_t offset = 0;

        while (offset < size) {
            const ssize_t received = ::recv(
                socket_,
                output + offset,
                size - offset,
                0);

            if (received > 0) {
                offset += static_cast<std::size_t>(received);
                continue;
            }

            if (received < 0 && errno == EINTR) {
                continue;
            }

            close();
            return false;
        }

        return true;
    }

private:
    int socket_ = -1;

    static void configure_socket(int socket_fd) {
        timeval timeout{};
        timeout.tv_sec = kIoTimeoutSeconds;
        timeout.tv_usec = 0;

        setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout));
        setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &timeout,
            sizeof(timeout));

#ifdef SO_NOSIGPIPE
        int enabled = 1;
        setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_NOSIGPIPE,
            &enabled,
            sizeof(enabled));
#endif
    }
};

}  // namespace

std::unique_ptr<DiscordIpcTransport> create_discord_ipc_transport() {
    return std::make_unique<UnixDiscordIpcTransport>();
}

}  // namespace nso

#endif
