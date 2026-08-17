#include "nso_album_sync/coral.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace nso {
namespace {

constexpr char kCoralBaseUrl[] =
    "https://api-lp1.znc.srv.nintendo.net";
constexpr char kLoginPath[] = "/v4/Account/Login";
constexpr char kMediaListPath[] = "/v4/Media/List";
constexpr char kShowSelfPath[] = "/v4/User/ShowSelf";

constexpr std::size_t kMaxCoralAuthAttemptsPerHour = 3;

std::string upper(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return text;
}

std::string coral_url(const char* path) {
    return std::string(kCoralBaseUrl) + path;
}

MediaItem parse_media_item(const Json& item) {
    MediaItem media;
    media.id = item.string("id");
    media.title_id = item.string("titleId", item.string("applicationId"));
    media.app_name = item.string("appName", "Nintendo Switch");
    media.type = item.string("type", "image");
    media.captured_at = item.integer("capturedAt");
    media.uploaded_at = item.integer("uploadedAt");
    media.expires_at = item.integer("expiresAt");
    media.content_uri = item.string("contentUri");
    media.thumbnail_uri = item.string("thumbnailUri");
    return media;
}

NintendoPresence parse_presence(const Json& result) {
    NintendoPresence presence;

    const auto* presence_json = result.find("presence");
    if (presence_json == nullptr) {
        return presence;
    }

    presence.state = presence_json->string("state", "OFFLINE");
    presence.updated_at = presence_json->integer("updatedAt");

    if (const auto* platform = presence_json->find("platform")) {
        if (platform->is_string()) {
            presence.platform = platform->as_string();
        } else if (platform->is_number()) {
            presence.platform = std::to_string(platform->as_i64());
        }
    }

    if (const auto* game = presence_json->find("game")) {
        presence.game_name = game->string("name");
        presence.image_uri = game->string("imageUri");
        presence.shop_uri = game->string("shopUri");
        presence.sys_description = game->string("sysDescription");
        presence.total_play_time = game->integer("totalPlayTime");
    }

    return presence;
}

}  // namespace

std::string NintendoPresence::console_name() const {
    const auto normalized = upper(platform);

    const bool is_switch_2 =
        normalized == "2" ||
        normalized == "OUNCE" ||
        normalized == "SWITCH_2" ||
        normalized == "SWITCH2" ||
        normalized == "NINTENDO_SWITCH_2";

    return is_switch_2 ? "Nintendo Switch 2" : "Nintendo Switch";
}

std::string NintendoPresence::discord_state() const {
    if (!sys_description.empty()) {
        return sys_description;
    }

    const auto played_hours = total_play_time / 60;
    if (played_hours < 5) {
        return "Played for a little while";
    }

    // Nintendo reports play time in five-hour buckets in this UI.
    const auto rounded_hours = (played_hours / 5) * 5;
    return "Played for " + std::to_string(rounded_hours) + " hours or more";
}

std::string CoralClient::ensure_session(const std::string& session_token) {
    std::lock_guard lock(session_mutex_);
    const auto now = Clock::now();

    const bool cached_session_is_valid =
        !coral_access_token_.empty() &&
        cached_session_token_ == session_token &&
        now < coral_token_expiry_;

    if (cached_session_is_valid) {
        return coral_access_token_;
    }

    while (!auth_attempts_.empty() &&
           now - auth_attempts_.front() > std::chrono::hours(1)) {
        auth_attempts_.pop_front();
    }

    if (auth_attempts_.size() >= kMaxCoralAuthAttemptsPerHour) {
        throw std::runtime_error(
            "Coral authentication paused locally to protect nxapi/Nintendo rate limits");
    }

    // Count the attempt before networking so repeated failures are also bounded.
    auth_attempts_.push_back(now);

    const auto nintendo_tokens = auth_.exchange_session_token(session_token);
    const auto profile = auth_.fetch_profile(nintendo_tokens.access_token);
    const auto encrypted_login =
        nxapi_.encrypted_login_body(nintendo_tokens.id_token, profile);
    const auto nso_version = nxapi_.nso_version();

    const auto response = http_.post_bytes(
        coral_url(kLoginPath),
        encrypted_login,
        {
            "X-Platform: Android",
            "X-ProductVersion: " + nso_version,
            "User-Agent: com.nintendo.znca/" + nso_version + "(Android/12)",
        });

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Coral login failed (HTTP " + std::to_string(response.status) + ")");
    }

    const auto login = Json::parse(nxapi_.decrypt_response(response.body));
    const auto* result = login.find("result");
    if (result == nullptr) {
        throw std::runtime_error("Coral login missing result");
    }

    const auto* credential = result->find("webApiServerCredential");
    if (credential == nullptr) {
        throw std::runtime_error("Coral login missing credential");
    }

    coral_access_token_ = credential->string("accessToken");
    if (coral_access_token_.empty()) {
        throw std::runtime_error("Coral login missing access token");
    }

    if (const auto* user = result->find("user")) {
        user_id_ = user->string("id", user->string("nsaId"));
    }

    cached_session_token_ = session_token;

    const auto expires_in = credential->integer("expiresIn", 7200);
    coral_token_expiry_ =
        now + std::chrono::seconds(std::max<std::int64_t>(1, expires_in - 10));

    return coral_access_token_;
}

Json CoralClient::coral_call(
    const std::string& url,
    const std::string& access_token,
    const std::string& request_body) {
    const auto nso_version = nxapi_.nso_version();
    const auto encrypted_request =
        nxapi_.encrypt_request(url, access_token, request_body);

    const auto response = http_.post_bytes(
        url,
        encrypted_request,
        {
            "Authorization: Bearer " + access_token,
            "User-Agent: com.nintendo.znca/" + nso_version + "(Android/12)",
        });

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Coral request failed (HTTP " + std::to_string(response.status) + ")");
    }

    return Json::parse(nxapi_.decrypt_response(response.body));
}

std::vector<MediaItem> CoralClient::media_list(
    const std::string& session_token) {
    const auto access_token = ensure_session(session_token);
    const auto response = coral_call(
        coral_url(kMediaListPath),
        access_token,
        R"({"parameter":{}})");

    std::vector<MediaItem> media;

    const auto* result = response.find("result");
    if (result == nullptr) {
        return media;
    }

    const auto* media_json = result->find("media");
    if (media_json == nullptr || !media_json->is_array()) {
        return media;
    }

    media.reserve(media_json->as_array().size());
    for (const auto& item : media_json->as_array()) {
        media.push_back(parse_media_item(item));
    }

    return media;
}

NintendoPresence CoralClient::self_presence(
    const std::string& session_token) {
    const auto access_token = ensure_session(session_token);

    const std::string request_body = user_id_.empty()
        ? R"({"parameter":{}})"
        : R"({"parameter":{"id":)" + user_id_ + "}}";

    const auto response = coral_call(
        coral_url(kShowSelfPath),
        access_token,
        request_body);

    const auto* result = response.find("result");
    if (result == nullptr) {
        return {};
    }

    return parse_presence(*result);
}

}  // namespace nso
