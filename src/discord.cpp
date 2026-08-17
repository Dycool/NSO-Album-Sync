#include "nso_album_sync/discord.hpp"

#include "nso_album_sync/discord_ipc.hpp"
#include "nso_album_sync/json.hpp"

#include <memory>
#include <string>

namespace nso {
namespace {

Json activity_from_presence(const NintendoPresence& presence) {
    Json::object activity{
        {"type", 0},
        {"details", presence.game_name},
        {"instance", false},
    };

    std::string state = presence.console_name();
    const auto play_time = presence.discord_state();
    if (!play_time.empty()) {
        if (!state.empty()) {
            state += " · ";
        }
        state += play_time;
    }

    if (!state.empty()) {
        activity.emplace("state", std::move(state));
    }

    Json::object assets;
    if (!presence.image_uri.empty()) {
        assets.emplace("large_image", presence.image_uri);
        assets.emplace("large_text", presence.game_name);
    }
    if (!presence.shop_uri.empty()) {
        assets.emplace("large_url", presence.shop_uri);
    }

    if (!assets.empty()) {
        activity.emplace("assets", std::move(assets));
    }

    return Json(std::move(activity));
}

}  // namespace

struct DiscordPresence::Impl {
    explicit Impl(std::uint64_t application_id)
        : ipc(application_id) {}

    DiscordIpcClient ipc;
};

DiscordPresence::DiscordPresence(std::uint64_t application_id)
    : impl_(std::make_unique<Impl>(application_id)) {}

DiscordPresence::~DiscordPresence() {
    clear();
}

bool DiscordPresence::available() const {
    // Raw Discord IPC is built into the application. Discord itself does not
    // need to be running for the feature to be available in the tray menu.
    return true;
}

void DiscordPresence::clear() {
    if (impl_) {
        impl_->ipc.clear_activity();
    }
}

void DiscordPresence::update(const NintendoPresence& presence) {
    if (!impl_) {
        return;
    }

    if (!presence.is_playing()) {
        clear();
        return;
    }

    // Presence is optional: failure simply means Discord is not running or the
    // local IPC request was rejected. Nintendo album synchronization continues.
    impl_->ipc.set_activity(activity_from_presence(presence));
}

}  // namespace nso
