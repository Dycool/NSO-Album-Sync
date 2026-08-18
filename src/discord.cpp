#include "nso_album_sync/discord.hpp"

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#endif

namespace nso {
namespace {

constexpr auto kCallbackPumpInterval = std::chrono::milliseconds(100);

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
    // The SDK is compiled into/bundled with the application. Construct the
    // Discord client lazily only after the user enables Rich Presence.
    return impl_ != nullptr;
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

    // Social SDK 1.6+ allows the activity name itself to be customized. Use
    // that name as the status display field so Discord renders the game as the
    // primary activity identity instead of repeating our publisher app name.
    activity.SetName(presence.game_name);
    activity.SetStatusDisplayType(discordpp::StatusDisplayTypes::Name);
    activity.SetState(presence.console_name());

    if (!presence.image_uri.empty()) {
        discordpp::ActivityAssets assets;
        assets.SetLargeImage(presence.image_uri);
        assets.SetLargeText(presence.game_name);
        if (!presence.shop_uri.empty()) assets.SetLargeUrl(presence.shop_uri);
        activity.SetAssets(assets);
    }

    impl_->set_callback_pump(true);
    impl_->client->UpdateRichPresence(
        std::move(activity),
        [](discordpp::ClientResult) {});
}

}  // namespace nso
