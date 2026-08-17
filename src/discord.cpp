#include "nso_album_sync/discord.hpp"

#include <string>

#ifdef NSO_DISCORD_SOCIAL_SDK
#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#endif

namespace nso {

struct DiscordPresence::Impl {
#ifdef NSO_DISCORD_SOCIAL_SDK
    std::shared_ptr<discordpp::Client> client;
#endif
    bool ready = false;
};

DiscordPresence::DiscordPresence(std::uint64_t application_id)
    : impl_(std::make_unique<Impl>()) {
#ifdef NSO_DISCORD_SOCIAL_SDK
    try {
        impl_->client = std::make_shared<discordpp::Client>();
        impl_->client->SetApplicationId(application_id);
        impl_->ready = true;
    } catch (...) {
        impl_->ready = false;
    }
#else
    (void)application_id;
#endif
}

DiscordPresence::~DiscordPresence() {
    clear();
}

bool DiscordPresence::available() const {
    return impl_->ready;
}

void DiscordPresence::clear() {
#ifdef NSO_DISCORD_SOCIAL_SDK
    if (!impl_->ready || !impl_->client) {
        return;
    }

    impl_->client->ClearRichPresence();
    discordpp::RunCallbacks();
#endif
}

void DiscordPresence::update(const NintendoPresence& presence) {
#ifdef NSO_DISCORD_SOCIAL_SDK
    if (!impl_->ready || !impl_->client) {
        return;
    }

    if (!presence.is_playing()) {
        clear();
        return;
    }

    discordpp::Activity activity;
    activity.SetType(discordpp::ActivityTypes::Playing);
    activity.SetDetails(presence.game_name);

    std::string state = presence.console_name();
    const auto play_time = presence.discord_state();
    if (!play_time.empty()) {
        state += " · " + play_time;
    }
    activity.SetState(state);

    discordpp::ActivityAssets assets;
    if (!presence.image_uri.empty()) {
        assets.SetLargeImage(presence.image_uri);
    }
    assets.SetLargeText(presence.game_name);
    if (!presence.shop_uri.empty()) {
        assets.SetLargeUrl(presence.shop_uri);
    }
    activity.SetAssets(assets);

    impl_->client->UpdateRichPresence(
        activity,
        [](discordpp::ClientResult) {
            // Presence failures are intentionally non-fatal to album sync.
        });
    discordpp::RunCallbacks();
#else
    (void)presence;
#endif
}

}  // namespace nso
