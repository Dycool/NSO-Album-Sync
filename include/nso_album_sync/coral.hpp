#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nso {

struct MediaItem {
    std::string id;
    std::string title_id;
    std::string app_name;
    std::string type;
    std::string content_uri;
    std::string thumbnail_uri;
    std::int64_t content_length = 0;
    std::int64_t captured_at = 0;
    std::int64_t uploaded_at = 0;
    std::int64_t expires_at = 0;
};

struct NintendoPresence {
    std::string state = "OFFLINE";
    std::string platform;
    std::string user_name;
    std::string game_name;
    std::string title_id;
    std::string image_uri;
    std::string shop_uri;
    std::string sys_description;
    std::int64_t updated_at = 0;
    std::int64_t total_play_time = 0;

    std::string custom_state;
    std::string custom_details;
    std::string custom_image_uri;
    std::string custom_large_image_uri;
    std::string custom_large_text;

    bool is_playing() const { return state == "ONLINE" || state == "PLAYING"; }
    std::string console_name() const;
    std::string discord_state() const;
};

class CoralClient {
public:
    CoralClient(
        HttpClient& http,
        NintendoAuthManager& auth,
        NxapiClient& nxapi,
        std::filesystem::path cache_directory)
        : http_(http),
          auth_(auth),
          nxapi_(nxapi),
          cache_directory_(std::move(cache_directory)) {}

    std::vector<MediaItem> media_list(const std::string& session_token);
    NintendoPresence self_presence(const std::string& session_token);
    std::string get_web_service_token(
        const std::string& session_token,
        std::uint64_t game_service_id = 0);
    void clear_cached_session();

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    NintendoAuthManager& auth_;
    NxapiClient& nxapi_;
    std::filesystem::path cache_directory_;

    std::mutex session_mutex_;
    std::mutex login_mutex_;
    std::mutex request_mutex_;
    mutable std::recursive_mutex rate_limit_mutex_;
    std::atomic<std::uint64_t> session_generation_{0};
    std::string coral_access_token_;
    std::string cached_session_token_;
    std::string user_id_;
    std::string na_id_;
    Clock::time_point coral_token_expiry_{};
    std::deque<Clock::time_point> auth_attempts_;
    std::string rate_limit_session_hash_;
    Clock::time_point coral_rate_limit_until_{};

    struct CachedWebServiceToken {
        std::string token;
        Clock::time_point expires_at{};
    };
    std::unordered_map<std::uint64_t, CachedWebServiceToken> web_service_tokens_;

    std::string ensure_session(const std::string& session_token);
    Json coral_call(
        const std::string& url,
        const std::string& access_token,
        const std::string& request_body);

    bool restore_persistent_session(
        const std::string& session_token,
        Clock::time_point now);
    void persist_session(const std::string& session_token);
    void load_auth_attempts(const std::string& session_token, Clock::time_point now);
    void save_auth_attempts() const;
    void throw_if_coral_rate_limited(Clock::time_point now) const;
    void apply_coral_rate_limit_response(const HttpResponse& response);
};

}  // namespace nso
