#include "nso_album_sync/discord.hpp"

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#endif

#ifndef NSO_DISCORD_SOCIAL_SDK_VERSION
#define NSO_DISCORD_SOCIAL_SDK_VERSION "1.10.18687"
#endif
#ifndef NSO_DISCORD_SDK_RESOURCE_ID
#define NSO_DISCORD_SDK_RESOURCE_ID 101
#endif

namespace nso {
namespace {

constexpr auto kCallbackPumpInterval = std::chrono::milliseconds(100);
constexpr std::size_t kDiscordActivityImageMaxLength = 300;
constexpr char kNookLinkOrigin[] = "https://web.sd.lp1.acbaa.srv.nintendo.net";

#ifdef _WIN32
bool ensure_embedded_discord_sdk_loaded() {
    static const bool loaded = [] {
        const HMODULE executable = GetModuleHandleW(nullptr);
        if (executable == nullptr) return false;

        const HRSRC resource = FindResourceW(
            executable,
            MAKEINTRESOURCEW(NSO_DISCORD_SDK_RESOURCE_ID),
            RT_RCDATA);
        if (resource == nullptr) return false;

        const HGLOBAL data_handle = LoadResource(executable, resource);
        if (data_handle == nullptr) return false;
        const auto* data = static_cast<const unsigned char*>(LockResource(data_handle));
        const DWORD size = SizeofResource(executable, resource);
        if (data == nullptr || size == 0) return false;

        std::error_code error;
        auto directory = std::filesystem::temp_directory_path(error);
        if (error) return false;
        directory /= "NSOAlbumSync";
        directory /= "discord-social-sdk-" NSO_DISCORD_SOCIAL_SDK_VERSION;
        std::filesystem::create_directories(directory, error);
        if (error) return false;

        const auto dll = directory / "discord_partner_sdk.dll";
        auto temporary = dll;
        temporary += ".tmp";
        {
            // Always refresh the extracted runtime from the bytes embedded in
            // this executable. This prevents a stale/tampered temp copy from a
            // previous launch from being trusted just because its size matches.
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(
                reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size));
            output.flush();
            if (!output) return false;
        }

        std::filesystem::rename(temporary, dll, error);
        if (error) {
            std::filesystem::remove(dll, error);
            error.clear();
            std::filesystem::rename(temporary, dll, error);
            if (error) {
                std::filesystem::remove(temporary, error);
                return false;
            }
        }

        // Load the exact embedded Discord runtime before the first delayed SDK
        // import is resolved. Keeping it loaded for the process lifetime avoids
        // dangling delay-import thunks during static destruction.
        return LoadLibraryW(dll.c_str()) != nullptr;
    }();
    return loaded;
}
#endif

bool is_valid_discord_image_url(const std::string& url) {
    if (url.empty() || url.size() > kDiscordActivityImageMaxLength) return false;
    if (url.rfind("https://", 0) != 0) return false;
    for (char c : url) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool is_animal_crossing_presence(const NintendoPresence& presence) {
    const auto& name = presence.game_name;
    return name.find("Animal Crossing") != std::string::npos ||
        name.find("あつまれ どうぶつの森") != std::string::npos;
}

std::string normalize_discord_image_url(
    const NintendoPresence& presence,
    std::string url) {
    if (url.empty()) return {};

    // NookLink's browser can use relative passport-photo URLs because the page
    // itself runs on web.sd. Discord activity assets cannot: they require an
    // absolute HTTPS URL. Resolve both root-relative and ordinary relative
    // NookLink image values before applying Discord's 300-character limit.
    if (is_animal_crossing_presence(presence)) {
        if (url.rfind("//", 0) == 0) {
            url = "https:" + url;
        } else if (url.rfind("/", 0) == 0) {
            url = std::string(kNookLinkOrigin) + url;
        } else if (url.find("://") == std::string::npos &&
                   url.rfind("data:", 0) != 0) {
            url = std::string(kNookLinkOrigin) + "/" + url;
        }
    }

    return url;
}

void log_rejected_custom_image(const NintendoPresence& presence, const std::string& url) {
    if (url.empty()) return;
    std::string reason;
    if (url.size() > kDiscordActivityImageMaxLength) {
        reason = "URL too long (" + std::to_string(url.size()) + " chars)";
    } else if (url.rfind("https://", 0) != 0) {
        reason = "not an HTTPS URL";
    } else {
        for (char c : url) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                reason = "contains whitespace";
                break;
            }
        }
    }
    if (reason.empty()) return;

    // Do not log the avatar URL itself; it may contain a user-specific image
    // identifier or signature. Scheme/length classification is enough to debug
    // Discord asset rejection safely.
    std::cerr << "[DiscordPresence] Custom image rejected for "
              << presence.game_name << ": " << reason << '\n';
}

}  // namespace

struct DiscordPresence::Impl {
    explicit Impl(std::uint64_t application_id)
        : application_id(application_id) {}

    ~Impl() {
        {
            std::lock_guard sdk_lock(sdk_mutex);
            if (client) client->ClearRichPresence();
        }
        {
            std::lock_guard lock(callback_mutex);
            stopping = true;
            callback_pump_enabled = false;
        }
        callback_cv.notify_all();
        if (callback_thread.joinable()) callback_thread.join();
        client.reset();
    }

    bool ensure_client_locked() {
        if (client) return true;
#ifdef _WIN32
        if (!ensure_embedded_discord_sdk_loaded()) return false;
#endif
        try {
            client = std::make_shared<discordpp::Client>();
            client->SetApplicationId(application_id);
            callback_thread = std::thread([this] { callback_loop(); });
            return true;
        } catch (...) {
            client.reset();
            return false;
        }
    }

    void set_callback_pump(bool enabled) {
        {
            std::lock_guard lock(callback_mutex);
            callback_pump_enabled = enabled;
        }
        callback_cv.notify_all();
    }

    void callback_loop() {
        std::unique_lock lock(callback_mutex);
        while (!stopping) {
            callback_cv.wait(lock, [this] {
                return stopping || callback_pump_enabled;
            });
            if (stopping) break;

            lock.unlock();
            {
                std::lock_guard sdk_lock(sdk_mutex);
                discordpp::RunCallbacks();
            }
            lock.lock();
            callback_cv.wait_for(lock, kCallbackPumpInterval, [this] {
                return stopping || !callback_pump_enabled;
            });
        }
    }

    std::uint64_t application_id = 0;
    std::shared_ptr<discordpp::Client> client;
    std::mutex sdk_mutex;
    std::thread callback_thread;
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_pump_enabled = false;
    bool stopping = false;
};

DiscordPresence::DiscordPresence(std::uint64_t application_id)
    : impl_(std::make_unique<Impl>(application_id)) {}

DiscordPresence::~DiscordPresence() {
    clear();
}

bool DiscordPresence::available() const {
    return impl_ != nullptr;
}

bool DiscordPresence::self_test_runtime() {
    if (!impl_) return false;
    std::lock_guard sdk_lock(impl_->sdk_mutex);
    return impl_->ensure_client_locked();
}

void DiscordPresence::clear() {
    if (!impl_) return;
    {
        std::lock_guard sdk_lock(impl_->sdk_mutex);
        if (!impl_->client) return;
        impl_->client->ClearRichPresence();
    }
    impl_->set_callback_pump(false);
}

void DiscordPresence::update(const NintendoPresence& presence) {
    if (!impl_) return;
    if (!presence.is_playing()) {
        clear();
        return;
    }

    std::unique_lock sdk_lock(impl_->sdk_mutex);
    if (!impl_->ensure_client_locked()) return;

    discordpp::Activity activity;
    activity.SetType(discordpp::ActivityTypes::Playing);

    // Activity name is set to the currently played game title
    activity.SetName(presence.game_name);
    activity.SetStatusDisplayType(discordpp::StatusDisplayTypes::Name);

    if (!presence.custom_details.empty()) {
        activity.SetDetails(presence.custom_details);
    } else {
        const auto playtime_description = presence.discord_state();
        if (!playtime_description.empty()) {
            activity.SetDetails(playtime_description);
        }
    }

    if (!presence.custom_state.empty()) {
        activity.SetState(presence.custom_state);
    } else {
        activity.SetState(presence.console_name());
    }

    if (presence.updated_at > 0) {
        const auto start_seconds = presence.updated_at > 10'000'000'000LL
            ? presence.updated_at / 1000
            : presence.updated_at;
        discordpp::ActivityTimestamps timestamps;
        timestamps.SetStart(static_cast<std::uint64_t>(start_seconds));
        activity.SetTimestamps(timestamps);
    }

    // Coral's current-game artwork is always the primary image. Game-service
    // enrichment (weapon/avatar/fighter/etc.) is supplementary and therefore
    // belongs in Discord's small-image slot instead of replacing the game art.
    const auto large_image_uri = normalize_discord_image_url(presence, presence.image_uri);
    const auto small_image_uri = normalize_discord_image_url(presence, presence.custom_image_uri);
    if (!presence.custom_image_uri.empty() &&
        !is_valid_discord_image_url(small_image_uri)) {
        log_rejected_custom_image(presence, small_image_uri);
    }

    if (is_valid_discord_image_url(large_image_uri) ||
        is_valid_discord_image_url(small_image_uri)) {
        discordpp::ActivityAssets assets;
        if (is_valid_discord_image_url(large_image_uri)) {
            assets.SetLargeImage(large_image_uri);
            assets.SetLargeText(presence.game_name);
            if (is_valid_discord_image_url(presence.shop_uri)) {
                assets.SetLargeUrl(presence.shop_uri);
            }
        }
        if (is_valid_discord_image_url(small_image_uri)) {
            assets.SetSmallImage(small_image_uri);
        }
        activity.SetAssets(assets);
    }

    impl_->set_callback_pump(true);
    impl_->client->UpdateRichPresence(
        std::move(activity),
        [](const discordpp::ClientResult& result) {
            if (!result.Successful()) {
                std::cerr << "[DiscordPresence] UpdateRichPresence failed: "
                          << result.Error() << "\n";
            }
        });
}

}  // namespace nso
