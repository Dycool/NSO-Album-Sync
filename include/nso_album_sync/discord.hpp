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
    std::unique_ptr<Impl> impl_;
};

}  // namespace nso
