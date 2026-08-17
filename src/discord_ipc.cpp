#include "nso_album_sync/discord_ipc.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <string>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#else
#include <unistd.h>
#endif

namespace nso {
namespace {

constexpr std::uint32_t kRpcVersion = 1;
constexpr std::size_t kFrameHeaderSize = 8;
constexpr std::uint32_t kMaximumPayloadSize = 1024 * 1024;

void write_u32_le(std::uint8_t* destination, std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value & 0xffU);
    destination[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    destination[2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    destination[3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

std::uint32_t read_u32_le(const std::uint8_t* source) {
    return static_cast<std::uint32_t>(source[0]) |
           (static_cast<std::uint32_t>(source[1]) << 8U) |
           (static_cast<std::uint32_t>(source[2]) << 16U) |
           (static_cast<std::uint32_t>(source[3]) << 24U);
}

std::int64_t current_process_id() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

bool is_error_response(const Json& response) {
    return response.string("evt") == "ERROR";
}

}  // namespace

DiscordIpcClient::DiscordIpcClient(std::uint64_t application_id)
    : application_id_(std::to_string(application_id)),
      transport_(create_discord_ipc_transport()) {}

DiscordIpcClient::~DiscordIpcClient() {
    std::lock_guard lock(mutex_);
    disconnect_locked();
}

bool DiscordIpcClient::connected() const {
    std::lock_guard lock(mutex_);
    return ready_ && transport_ && transport_->connected();
}

bool DiscordIpcClient::set_activity(const Json& activity) {
    std::lock_guard lock(mutex_);
    return send_activity_locked(activity);
}

void DiscordIpcClient::clear_activity() {
    std::lock_guard lock(mutex_);

    // Discord clears Rich Presence automatically when the IPC connection dies.
    // If there is no live connection, opening one solely to clear an already
    // absent activity is unnecessary.
    if (!ready_ || !transport_ || !transport_->connected()) {
        return;
    }

    if (!send_activity_once_locked(Json(nullptr))) {
        disconnect_locked();
    }
}

bool DiscordIpcClient::ensure_connected_locked() {
    if (ready_ && transport_ && transport_->connected()) {
        return true;
    }

    disconnect_locked();

    if (!transport_) {
        transport_ = create_discord_ipc_transport();
    }

    if (!transport_ || !transport_->connect()) {
        return false;
    }

    const Json handshake(Json::object{
        {"v", static_cast<std::int64_t>(kRpcVersion)},
        {"client_id", application_id_},
    });

    if (!send_frame_locked(Opcode::handshake, handshake.dump()) ||
        !wait_for_ready_locked()) {
        disconnect_locked();
        return false;
    }

    ready_ = true;
    return true;
}

void DiscordIpcClient::disconnect_locked() {
    ready_ = false;
    if (transport_) {
        transport_->close();
    }
}

bool DiscordIpcClient::send_frame_locked(
    Opcode opcode,
    const std::string& payload) {
    if (!transport_ || !transport_->connected()) {
        return false;
    }

    if (payload.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }

    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload.size());
    write_u32_le(frame.data(), static_cast<std::uint32_t>(opcode));
    write_u32_le(
        frame.data() + 4,
        static_cast<std::uint32_t>(payload.size()));

    if (!payload.empty()) {
        std::memcpy(
            frame.data() + kFrameHeaderSize,
            payload.data(),
            payload.size());
    }

    return transport_->write_frame(frame);
}

bool DiscordIpcClient::read_frame_locked(Frame& frame) {
    if (!transport_ || !transport_->connected()) {
        return false;
    }

    std::array<std::uint8_t, kFrameHeaderSize> header{};
    if (!transport_->read_exact(header.data(), header.size())) {
        return false;
    }

    const auto opcode_value = read_u32_le(header.data());
    const auto payload_size = read_u32_le(header.data() + 4);

    if (payload_size > kMaximumPayloadSize || opcode_value > 4) {
        return false;
    }

    frame.opcode = static_cast<Opcode>(opcode_value);
    frame.payload.assign(payload_size, '\0');

    if (payload_size > 0 &&
        !transport_->read_exact(frame.payload.data(), payload_size)) {
        return false;
    }

    return true;
}

bool DiscordIpcClient::read_rpc_frame_locked(Json& payload) {
    // Normally the next frame is the lock-step response to our command. Handle
    // Discord keepalive pings transparently so they cannot desynchronize us.
    for (int attempts = 0; attempts < 8; ++attempts) {
        Frame frame;
        if (!read_frame_locked(frame)) {
            return false;
        }

        if (frame.opcode == Opcode::ping) {
            if (!send_frame_locked(Opcode::pong, frame.payload)) {
                return false;
            }
            continue;
        }

        if (frame.opcode == Opcode::close) {
            return false;
        }

        if (frame.opcode != Opcode::frame) {
            continue;
        }

        try {
            payload = Json::parse(frame.payload);
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

bool DiscordIpcClient::wait_for_ready_locked() {
    for (int attempts = 0; attempts < 8; ++attempts) {
        Json response;
        if (!read_rpc_frame_locked(response)) {
            return false;
        }

        if (is_error_response(response)) {
            return false;
        }

        if (response.string("cmd") == "DISPATCH" &&
            response.string("evt") == "READY") {
            return true;
        }
    }

    return false;
}

bool DiscordIpcClient::send_activity_locked(const Json& activity) {
    if (!ensure_connected_locked()) {
        return false;
    }

    if (send_activity_once_locked(activity)) {
        return true;
    }

    // Discord may have restarted since the previous presence update. Reconnect
    // once and retry this local IPC command; there is no network/API retry loop.
    disconnect_locked();
    return ensure_connected_locked() && send_activity_once_locked(activity);
}

bool DiscordIpcClient::send_activity_once_locked(const Json& activity) {
    const std::string nonce = std::to_string(++nonce_);

    const Json command(Json::object{
        {"cmd", "SET_ACTIVITY"},
        {"args",
         Json::object{
             {"pid", current_process_id()},
             {"activity", activity},
         }},
        {"nonce", nonce},
    });

    if (!send_frame_locked(Opcode::frame, command.dump())) {
        return false;
    }

    // RPC echoes each command back. Consume that response before sending the
    // next update so the connection remains lock-step and cannot be flooded.
    for (int attempts = 0; attempts < 8; ++attempts) {
        Json response;
        if (!read_rpc_frame_locked(response)) {
            return false;
        }

        if (is_error_response(response)) {
            return false;
        }

        if (response.string("nonce") == nonce) {
            return response.string("cmd") == "SET_ACTIVITY";
        }
    }

    return false;
}

}  // namespace nso
