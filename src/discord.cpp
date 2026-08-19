#include "nso_album_sync/discord.hpp"
#include "nso_album_sync/zeldanotes.hpp"

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
constexpr char kSplatNet2Origin[] = "https://app.splatoon2.nintendo.net";

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
    return presence.title_id == "01006f8002326000" ||
        name.find("Animal Crossing") != std::string::npos ||
        name.find("あつまれ どうぶつの森") != std::string::npos;
}

bool is_splatoon2_presence(const NintendoPresence& presence) {
    const auto& name = presence.game_name;
    return presence.title_id == "01003bc0000a0000" ||
        name.find("Splatoon 2") != std::string::npos ||
        name.find("スプラトゥーン2") != std::string::npos;
}

bool is_zelda_notes_presence(const NintendoPresence& presence) {
    if (zelda_notes_game_for_title_id(presence.title_id) != ZeldaNotesGame::Unknown) {
        return true;
    }
    const auto& name = presence.game_name;
    return name.find("Breath of the Wild") != std::string::npos ||
        name.find("ブレス オブ ザ ワイルド") != std::string::npos ||
        name.find("Tears of the Kingdom") != std::string::npos ||
        name.find("ティアーズ オブ ザ キングダム") != std::string::npos;
}

NintendoPresence with_live_zelda_location(const NintendoPresence& base) {
    auto effective = base;
    if (!is_zelda_notes_presence(base)) return effective;

    const auto live = zelda_notes_current_live_presence();
    if (!live.active) return effective;

    // Zelda owns only the two free-form text rows. Keep Coral's game name,
    // artwork, shop link, timestamp and Nintendo profile small image unchanged.
    const auto details = live.format_details();
    const auto state = live.format_state();
    if (!details.empty()) effective.custom_details = details;
    if (!state.empty()) effective.custom_state = state;
    return effective;
}

std::string normalize_discord_image_url(
    const NintendoPresence& presence,
    std::string url) {
    if (url.empty()) return {};

    const char* relative_origin = nullptr;
    if (is_animal_crossing_presence(presence)) {
        relative_origin = kNookLinkOrigin;
    } else if (is_splatoon2_presence(presence)) {
        relative_origin = kSplatNet2Origin;
    }

    // Browser-based game services can return root-relative asset paths because
    // their WebViews resolve them against the current service origin. Discord
    // activity assets cannot, so resolve known NookLink/SplatNet 2 paths first.
    if (relative_origin != nullptr) {
        if (url.rfind("//", 0) == 0) {
            url = "https:" + url;
        } else if (url.rfind("/", 0) == 0) {
            url = std::string(relative_origin) + url;
        } else if (url.find("://") == std::string::npos &&
                   url.rfind("data:", 0) != 0) {
            url = std::string(relative_origin) + "/" + url;
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

    // Do not log the custom image URL itself; game-service image values can
    // contain user-specific identifiers or signatures. Classification is enough.
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

    void remember_base(const NintendoPresence& presence) {
        std::lock_guard lock(presence_mutex);
        last_base_presence = presence;
        has_last_base_presence = true;
    }

    void forget_base() {
        std::lock_guard lock(presence_mutex);
        last_base_presence = {};
        has_last_base_presence = false;
    }

    void refresh_zelda_overlay() {
        NintendoPresence base;
        {
            std::lock_guard lock(presence_mutex);
            if (!has_last_base_presence || !last_base_presence.is_playing()) return;
            base = last_base_presence;
        }
        publish(with_live_zelda_location(base));
    }

    void clear_sdk_presence() {
        {
            std::lock_guard sdk_lock(sdk_mutex);
            if (client) client->ClearRichPresence();
        }
        set_callback_pump(false);
    }

    void publish(const NintendoPresence& presence) {
        if (!presence.is_playing()) {
            clear_sdk_presence();
            return;
        }

        std::unique_lock sdk_lock(sdk_mutex);
        if (!ensure_client_locked()) return;

        discordpp::Activity activity;
        activity.SetType(discordpp::ActivityTypes::Playing);

        // Activity name is always the currently played Coral game title.
        activity.SetName(presence.game_name);
        activity.SetStatusDisplayType(discordpp::StatusDisplayTypes::Name);

        const bool animal_crossing = is_animal_crossing_presence(presence);

        if (!presence.custom_details.empty()) {
            activity.SetDetails(presence.custom_details);
        } else {
            const auto playtime_description = presence.discord_state();
            if (!playtime_description.empty()) {
                activity.SetDetails(playtime_description);
            }
        }

        // Animal Crossing uses its one-shot NookLink enrichment for the identity
        // line (resident • island), while the second line stays fresh from Coral.
        // Other games continue to use their game-specific custom state normally.
        if (animal_crossing && !presence.custom_details.empty()) {
            const auto playtime_description = presence.discord_state();
            activity.SetState(
                playtime_description.empty()
                    ? presence.console_name()
                    : playtime_description);
        } else if (!presence.custom_state.empty()) {
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

        // Coral's current-game artwork is always the primary image. Zelda live
        // location changes only the text rows, so the normal Nintendo profile
        // small image remains untouched as well.
        const auto large_image_uri = normalize_discord_image_url(
            presence, presence.image_uri);
        const auto small_image_uri = normalize_discord_image_url(
            presence, presence.custom_image_uri);
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

        set_callback_pump(true);
        client->UpdateRichPresence(
            std::move(activity),
            [](const discordpp::ClientResult& result) {
                if (!result.Successful()) {
                    std::cerr << "[DiscordPresence] UpdateRichPresence failed: "
                              << result.Error() << "\n";
                }
            });
    }

    std::uint64_t application_id = 0;
    std::shared_ptr<discordpp::Client> client;
    std::mutex sdk_mutex;
    std::thread callback_thread;
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_pump_enabled = false;
    bool stopping = false;

    std::mutex presence_mutex;
    NintendoPresence last_base_presence;
    bool has_last_base_presence = false;
};

DiscordPresence::DiscordPresence(std::uint64_t application_id)
    : impl_(std::make_shared<Impl>(application_id)) {
    const std::weak_ptr<Impl> weak_impl = impl_;
    zelda_notes_set_rpc_refresh_callback([weak_impl] {
        if (const auto impl = weak_impl.lock()) {
            impl->refresh_zelda_overlay();
        }
    });
}

DiscordPresence::~DiscordPresence() {
    // Unregister first. A callback that already copied the weak_ptr is still
    // safe: it can only keep Impl alive briefly, never the destroyed wrapper.
    zelda_notes_set_rpc_refresh_callback({});
    clear();
    impl_.reset();
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
    const auto impl = impl_;
    if (!impl) return;

    // Forget the Coral snapshot before stopping Zelda. stop_live_session() may
    // publish its empty fallback from another thread; that callback must not be
    // able to republish stale activity while clear() is in progress.
    impl->forget_base();
    zelda_notes_note_discord_presence({}, {}, false);
    impl->clear_sdk_presence();
}

void DiscordPresence::update(const NintendoPresence& presence) {
    const auto impl = impl_;
    if (!impl) return;
    if (!presence.is_playing()) {
        clear();
        return;
    }

    // Cache the untouched Coral/game-service presence. Zelda callbacks redraw
    // this snapshot locally; they never wake App::presence_loop() or cause an
    // extra /ShowSelf request.
    impl->remember_base(presence);

    // The existing one-shot Zelda branch has already cached its WebServiceToken
    // before the first Discord update for this play session. This call only
    // selects BOTW/TOTK and starts/stops the independent live map stream.
    zelda_notes_note_discord_presence(
        presence.title_id, presence.game_name, true);

    impl->publish(with_live_zelda_location(presence));
}

}  // namespace nso
