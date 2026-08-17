#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/nintendo_auth.hpp"
#include "nso_album_sync/nxapi.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nso {

struct MediaItem {
    std::string id;
    std::string title_id;
    std::string app_name;
    std::string type;
    std::string content_uri;
    std::string thumbnail_uri;

    std::int64_t captured_at = 0;
    std::int64_t uploaded_at = 0;
    std::int64_t expires_at = 0;
};

struct NintendoPresence {
    std::string state = "OFFLINE";
    std::string platform;
    std::string game_name;
    std::string image_uri;
    std::string shop_uri;
    std::string sys_description;

    std::int64_t updated_at = 0;
    std::int64_t total_play_time = 0;

    bool is_playing() const { return !game_name.empty(); }
    std::string console_name() const;
    std::string discord_state() const;
};

class CoralClient {
public:
    CoralClient(HttpClient& http, NintendoAuthManager& auth, NxapiClient& nxapi)
        : http_(http), auth_(auth), nxapi_(nxapi) {}

    std::vector<MediaItem> media_list(const std::string& session_token);
    NintendoPresence self_presence(const std::string& session_token);

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    NintendoAuthManager& auth_;
    NxapiClient& nxapi_;

    std::mutex session_mutex_;
    std::string coral_access_token_;
    std::string cached_session_token_;
    std::string user_id_;
    Clock::time_point coral_token_expiry_{};
    std::deque<Clock::time_point> auth_attempts_;

    std::string ensure_session(const std::string& session_token);
    Json coral_call(
        const std::string& url,
        const std::string& access_token,
        const std::string& request_body);
};

}  // namespace nso
