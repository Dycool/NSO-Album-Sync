#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef NSO_DISCORD_SOCIAL_SDK_VERSION
#define NSO_DISCORD_SOCIAL_SDK_VERSION "1.10.18687"
#endif
#ifndef NSO_DISCORD_SDK_RESOURCE_ID
#define NSO_DISCORD_SDK_RESOURCE_ID 201
#endif

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

        return LoadLibraryW(dll.c_str()) != nullptr;
    }();
    return loaded;
}
#endif

const char* nonnull(const char* value) {
    return value != nullptr ? value : "";
}

struct NativeActivity {
    const char* name;
    const char* details;
    const char* state;
    std::uint64_t start_seconds;
    const char* large_image;
    const char* large_text;
    const char* large_url;
    const char* small_image;
    const char* small_text;
};

class ClientHolder {
public:
    explicit ClientHolder(std::uint64_t application_id)
        : application_id_(application_id) {}

    ~ClientHolder() {
        {
            std::lock_guard sdk_lock(sdk_mutex_);
            if (client_) client_->ClearRichPresence();
        }
        {
            std::lock_guard lock(callback_mutex_);
            stopping_ = true;
            callback_pump_enabled_ = false;
        }
        callback_cv_.notify_all();
        if (callback_thread_.joinable()) callback_thread_.join();
        client_.reset();
    }

    bool self_test() {
        std::lock_guard sdk_lock(sdk_mutex_);
        return ensure_client_locked();
    }

    void clear() {
        {
            std::lock_guard sdk_lock(sdk_mutex_);
            if (client_) client_->ClearRichPresence();
        }
        set_callback_pump(false);
    }

    bool update(const NativeActivity& input) {
        std::unique_lock sdk_lock(sdk_mutex_);
        if (!ensure_client_locked()) return false;

        discordpp::Activity activity;
        activity.SetType(discordpp::ActivityTypes::Playing);
        activity.SetName(nonnull(input.name));
        activity.SetStatusDisplayType(discordpp::StatusDisplayTypes::Name);

        if (*nonnull(input.details) != '\0') {
            activity.SetDetails(input.details);
        }
        activity.SetState(nonnull(input.state));

        if (input.start_seconds > 0) {
            discordpp::ActivityTimestamps timestamps;
            timestamps.SetStart(input.start_seconds);
            activity.SetTimestamps(timestamps);
        }

        const bool has_large = *nonnull(input.large_image) != '\0';
        const bool has_small = *nonnull(input.small_image) != '\0';
        if (has_large || has_small) {
            discordpp::ActivityAssets assets;
            if (has_large) {
                assets.SetLargeImage(input.large_image);
                assets.SetLargeText(nonnull(input.large_text));
                if (*nonnull(input.large_url) != '\0') {
                    assets.SetLargeUrl(input.large_url);
                }
            }
            if (has_small) {
                assets.SetSmallImage(input.small_image);
                if (*nonnull(input.small_text) != '\0') {
                    assets.SetSmallText(input.small_text);
                }
            }
            activity.SetAssets(assets);
        }

        set_callback_pump(true);
        client_->UpdateRichPresence(
            std::move(activity),
            [](const discordpp::ClientResult& result) {
                if (!result.Successful()) {
                    std::cerr << "[DiscordPresence] UpdateRichPresence failed: "
                              << result.Error() << "\n";
                }
            });
        return true;
    }

private:
    bool ensure_client_locked() {
        if (client_) return true;
#ifdef _WIN32
        if (!ensure_embedded_discord_sdk_loaded()) return false;
#endif
        try {
            client_ = std::make_shared<discordpp::Client>();
            client_->SetApplicationId(application_id_);
            callback_thread_ = std::thread([this] { callback_loop(); });
            return true;
        } catch (...) {
            client_.reset();
            return false;
        }
    }

    void set_callback_pump(bool enabled) {
        {
            std::lock_guard lock(callback_mutex_);
            callback_pump_enabled_ = enabled;
        }
        callback_cv_.notify_all();
    }

    void callback_loop() {
        std::unique_lock lock(callback_mutex_);
        while (!stopping_) {
            callback_cv_.wait(lock, [this] {
                return stopping_ || callback_pump_enabled_;
            });
            if (stopping_) break;

            lock.unlock();
            {
                std::lock_guard sdk_lock(sdk_mutex_);
                discordpp::RunCallbacks();
            }
            lock.lock();
            callback_cv_.wait_for(lock, kCallbackPumpInterval, [this] {
                return stopping_ || !callback_pump_enabled_;
            });
        }
    }

    std::uint64_t application_id_ = 0;
    std::shared_ptr<discordpp::Client> client_;
    std::mutex sdk_mutex_;
    std::thread callback_thread_;
    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;
    bool callback_pump_enabled_ = false;
    bool stopping_ = false;
};
}  // namespace

extern "C" {

void* nso_discord_social_create(std::uint64_t application_id) noexcept {
    try {
        return new ClientHolder(application_id);
    } catch (...) {
        return nullptr;
    }
}

void nso_discord_social_destroy(void* handle) noexcept {
    try {
        delete static_cast<ClientHolder*>(handle);
    } catch (...) {
    }
}

int nso_discord_social_self_test(void* handle) noexcept {
    if (handle == nullptr) return 0;
    try {
        return static_cast<ClientHolder*>(handle)->self_test() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

void nso_discord_social_clear(void* handle) noexcept {
    if (handle == nullptr) return;
    try {
        static_cast<ClientHolder*>(handle)->clear();
    } catch (...) {
    }
}

int nso_discord_social_update(void* handle, const NativeActivity* activity) noexcept {
    if (handle == nullptr || activity == nullptr) return 0;
    try {
        return static_cast<ClientHolder*>(handle)->update(*activity) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
