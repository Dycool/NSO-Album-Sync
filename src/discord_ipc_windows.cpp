#include "nso_album_sync/discord_ipc.hpp"

#ifdef _WIN32

#include "nso_album_sync/windows_compat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace nso {
namespace {

constexpr auto kReadTimeout = std::chrono::seconds(2);

class WindowsDiscordIpcTransport final : public DiscordIpcTransport {
public:
    ~WindowsDiscordIpcTransport() override {
        close();
    }

    bool connect() override {
        close();

        for (int index = 0; index < 10; ++index) {
            const std::wstring path =
                L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(index);

            HANDLE pipe = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);

            if (pipe != INVALID_HANDLE_VALUE) {
                pipe_ = pipe;
                return true;
            }
        }

        return false;
    }

    void close() override {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    bool connected() const override {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    bool write_frame(const std::vector<std::uint8_t>& frame) override {
        if (!connected() || frame.empty()) {
            return false;
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(
            pipe_,
            frame.data(),
            static_cast<DWORD>(frame.size()),
            &written,
            nullptr);

        if (!ok || written != static_cast<DWORD>(frame.size())) {
            close();
            return false;
        }

        return true;
    }

    bool read_exact(void* data, std::size_t size) override {
        auto* output = static_cast<std::uint8_t*>(data);
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + kReadTimeout;

        while (offset < size) {
            if (!connected()) {
                return false;
            }

            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
                close();
                return false;
            }

            if (available == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const auto remaining = size - offset;
            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(remaining, available));
            DWORD received = 0;

            if (!ReadFile(
                    pipe_,
                    output + offset,
                    requested,
                    &received,
                    nullptr) ||
                received == 0) {
                close();
                return false;
            }

            offset += received;
        }

        return true;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace

std::unique_ptr<DiscordIpcTransport> create_discord_ipc_transport() {
    return std::make_unique<WindowsDiscordIpcTransport>();
}

}  // namespace nso

#endif
