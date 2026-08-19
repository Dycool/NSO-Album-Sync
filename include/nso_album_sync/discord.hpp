#pragma once

#include "nso_album_sync/coral.hpp"

#include <cstdint>
#include <memory>

namespace nso {

class DiscordPresence {
public:
    explicit DiscordPresence(std::uint64_t application_id);
    ~DiscordPresence();

    bool available() const;
    bool self_test_runtime();
    void update(const NintendoPresence& presence);
    void clear();

private:
    struct Impl;
    // Zelda Notes can request a Rich Presence redraw from its live SSE thread.
    // A shared implementation lets that callback hold only a weak_ptr, so a
    // late network callback can never dereference a destroyed DiscordPresence.
    std::shared_ptr<Impl> impl_;
};

}  // namespace nso
