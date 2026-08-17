#pragma once

#include "nso_album_sync/json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nso {

class DiscordIpcTransport {
public:
    virtual ~DiscordIpcTransport() = default;

    virtual bool connect() = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;
    virtual bool write_frame(const std::vector<std::uint8_t>& frame) = 0;
    virtual bool read_exact(void* data, std::size_t size) = 0;
};

std::unique_ptr<DiscordIpcTransport> create_discord_ipc_transport();

class DiscordIpcClient {
public:
    explicit DiscordIpcClient(std::uint64_t application_id);
    ~DiscordIpcClient();

    DiscordIpcClient(const DiscordIpcClient&) = delete;
    DiscordIpcClient& operator=(const DiscordIpcClient&) = delete;

    bool connected() const;
    bool set_activity(const Json& activity);
    void clear_activity();

private:
    enum class Opcode : std::uint32_t {
        handshake = 0,
        frame = 1,
        close = 2,
        ping = 3,
        pong = 4,
    };

    struct Frame {
        Opcode opcode = Opcode::frame;
        std::string payload;
    };

    std::string application_id_;
    std::unique_ptr<DiscordIpcTransport> transport_;
    mutable std::mutex mutex_;
    bool ready_ = false;
    std::uint64_t nonce_ = 0;

    bool ensure_connected_locked();
    void disconnect_locked();

    bool send_frame_locked(Opcode opcode, const std::string& payload);
    bool read_frame_locked(Frame& frame);
    bool read_rpc_frame_locked(Json& payload);
    bool wait_for_ready_locked();

    bool send_activity_locked(const Json& activity);
    bool send_activity_once_locked(const Json& activity);
};

}  // namespace nso
