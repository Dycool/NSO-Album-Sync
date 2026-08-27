#include "nso_album_sync/coral.hpp"

#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#else
#include <sys/stat.h>
#endif

namespace nso {
namespace {

constexpr char kCoralBaseUrl[] = "https://api-lp1.znc.srv.nintendo.net";
constexpr char kLoginPath[] = "/v4/Account/Login";
constexpr char kMediaListPath[] = "/v4/Media/List";
constexpr char kShowSelfPath[] = "/v4/User/ShowSelf";
constexpr char kCoralCredentialAccount[] = "CoralCredential";

// nxapi itself limits automated Coral authentication to four attempts per hour.
// Persist the attempt window so restarting the desktop app cannot bypass it.
constexpr std::size_t kMaxCoralAuthAttemptsPerHour = 4;
constexpr auto kDefaultCoralRateLimitBackoff = std::chrono::minutes(15);

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return text;
}

std::string coral_url(const char* path) {
    std::string url(kCoralBaseUrl);
    url.append(path);
    return url;
}

std::string session_hash(const std::string& session_token) {
    return base64url(sha256(session_token));
}

std::string exact_integer_field_in_object(
    const std::string& json,
    const std::string& object_key,
    const std::string& field_key) {
    const auto object_marker = "\"" + object_key + "\"";
    auto object_key_at = json.find(object_marker);
    while (object_key_at != std::string::npos) {
        auto colon = json.find(':', object_key_at + object_marker.size());
        if (colon == std::string::npos) return {};
        auto position = colon + 1;
        while (position < json.size() &&
               std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
        if (position >= json.size() || json[position] != '{') {
            object_key_at = json.find(object_marker, object_key_at + 1);
            continue;
        }

        int depth = 1;
        bool in_string = false;
        bool escaped = false;
        for (++position; position < json.size() && depth > 0; ++position) {
            const char ch = json[position];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
                continue;
            }
            if (ch == '"') {
                if (depth == 1 &&
                    json.compare(position, field_key.size() + 2,
                                 "\"" + field_key + "\"") == 0) {
                    auto value_colon = json.find(':', position + field_key.size() + 2);
                    if (value_colon == std::string::npos) return {};
                    auto value_at = value_colon + 1;
                    while (value_at < json.size() &&
                           std::isspace(static_cast<unsigned char>(json[value_at]))) {
                        ++value_at;
                    }
                    if (value_at < json.size() && json[value_at] == '"') {
                        const auto end = json.find('"', value_at + 1);
                        return end == std::string::npos
                            ? std::string{}
                            : json.substr(value_at + 1, end - value_at - 1);
                    }
                    const auto start = value_at;
                    if (value_at < json.size() && json[value_at] == '-') ++value_at;
                    while (value_at < json.size() &&
                           std::isdigit(static_cast<unsigned char>(json[value_at]))) {
                        ++value_at;
                    }
                    return value_at > start ? json.substr(start, value_at - start) : std::string{};
                }
                in_string = true;
            } else if (ch == '{' || ch == '[') {
                ++depth;
            } else if (ch == '}' || ch == ']') {
                --depth;
            }
        }
        object_key_at = json.find(object_marker, object_key_at + 1);
    }
    return {};
}

std::string auth_rate_limit_key(const std::string& session_token) {
    // Nintendo Account session tokens are JWTs. Match nxapi's persistent auth
    // limiter by Nintendo Account user (`sub`), not by the replaceable session
    // token, so signing out/in cannot reset the hourly attempt window.
    try {
        const auto first_dot = session_token.find('.');
        const auto second_dot = first_dot == std::string::npos
            ? std::string::npos
            : session_token.find('.', first_dot + 1);
        if (first_dot != std::string::npos && second_dot != std::string::npos) {
            const auto payload_bytes = base64_decode(
                session_token.substr(first_dot + 1, second_dot - first_dot - 1));
            const std::string payload(payload_bytes.begin(), payload_bytes.end());
            const auto subject = Json::parse(payload).string("sub");
            if (!subject.empty()) return base64url(sha256(subject));
        }
    } catch (...) {
    }
    // Safe compatibility fallback for malformed/legacy tokens.
    return session_hash(session_token);
}

std::int64_t epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point time_from_epoch(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

std::chrono::seconds retry_after_delay(const std::string& value) {
    try {
        const auto seconds = std::stoll(value);
        return std::chrono::seconds(std::max<std::int64_t>(1, seconds));
    } catch (...) {
    }

    std::tm parsed{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultCoralRateLimitBackoff);
    }
#ifdef _WIN32
    const std::time_t retry_at = _mkgmtime(&parsed);
#else
    const std::time_t retry_at = timegm(&parsed);
#endif
    if (retry_at <= 0) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultCoralRateLimitBackoff);
    }
    return std::chrono::seconds(std::max<std::int64_t>(
        1, static_cast<std::int64_t>(retry_at - std::time(nullptr))));
}

void replace_cache_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) std::filesystem::remove(temporary, error);
#endif
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
    media.content_length = item.integer("contentLength");
    return media;
}

NintendoPresence parse_presence(const Json& result) {
    NintendoPresence presence;
    // /v4/User/ShowSelf already includes the signed-in user's Nintendo/Coral
    // profile image and nickname/name. Use it as the generic Discord small image
    // and small text tooltip without another account/profile request.
    presence.user_name = result.string("name", result.string("nickname"));
    presence.custom_image_uri = result.string("imageUri", result.string("image2Uri"));
    const auto* presence_json = result.find("presence");
    if (presence_json == nullptr) return presence;

    presence.state = presence_json->string("state", "OFFLINE");
    presence.updated_at = presence_json->integer("updatedAt");
    if (const auto* platform = presence_json->find("platform")) {
        if (platform->is_string()) presence.platform = platform->as_string();
        else if (platform->is_number()) {
            presence.platform = std::to_string(platform->as_i64());
        }
    }
    if (const auto* game = presence_json->find("game")) {
        presence.game_name = game->string("name");
        presence.image_uri = game->string("imageUri");
        presence.shop_uri = game->string("shopUri");
        presence.sys_description = game->string("sysDescription");
        presence.total_play_time = game->integer("totalPlayTime");

        // 1. Direct titleId / applicationId fields
        presence.title_id = game->string("titleId", game->string("applicationId"));

        // 2. Numeric or string ID field in Coral
        if (presence.title_id.empty()) {
            if (const auto* id_val = game->find("id")) {
                if (id_val->is_string()) {
                    presence.title_id = id_val->as_string();
                } else if (id_val->is_number()) {
                    const auto numeric_id = static_cast<std::uint64_t>(id_val->as_i64());
                    if (numeric_id > 0) {
                        char hex_buf[32];
                        std::snprintf(hex_buf, sizeof(hex_buf), "%016llx", static_cast<unsigned long long>(numeric_id));
                        presence.title_id = hex_buf;
                    }
                }
            }
        }

        // 3. Extract 16-hex Title ID from shopUri (e.g. /apps/0100f2c0115b6000/US)
        if (presence.title_id.empty() && !presence.shop_uri.empty()) {
            const auto pos = presence.shop_uri.find("/apps/");
            if (pos != std::string::npos && pos + 6 + 16 <= presence.shop_uri.size()) {
                const auto candidate = presence.shop_uri.substr(pos + 6, 16);
                bool valid_hex = true;
                for (char c : candidate) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        valid_hex = false;
                        break;
                    }
                }
                if (valid_hex) {
                    presence.title_id = candidate;
                }
            }
        }

        presence.title_id = lower(presence.title_id);
    }
    return presence;
}

}  // namespace

std::string NintendoPresence::console_name() const {
    const auto normalized = upper(platform);
    const bool is_switch_2 =
        normalized == "2" || normalized == "OUNCE" ||
        normalized == "SWITCH_2" || normalized == "SWITCH2" ||
        normalized == "NINTENDO_SWITCH_2";
    return is_switch_2 ? "Nintendo Switch 2" : "Nintendo Switch";
}

std::string NintendoPresence::discord_state() const {
    if (!sys_description.empty()) return sys_description;
    if (total_play_time <= 0) return {};
    const auto played_hours = total_play_time / 60;
    if (played_hours < 5) return "Played for a little while";
    const auto rounded_hours = (played_hours / 5) * 5;
    return "Played for " + std::to_string(rounded_hours) + " hours or more";
}

bool CoralClient::restore_persistent_session(
    const std::string& session_token,
    Clock::time_point now) {
    if (!coral_access_token_.empty() || !SecureStore::available()) return false;
    const auto stored = SecureStore::get(kCoralCredentialAccount);
    if (!stored) return false;

    try {
        const auto json = Json::parse(*stored);
        if (json.string("sessionHash") != session_hash(session_token)) return false;
        const auto expires_at = time_from_epoch(json.integer("expiresAt"));
        const auto token = json.string("accessToken");
        if (token.empty() || now >= expires_at) {
            SecureStore::erase(kCoralCredentialAccount);
            return false;
        }

        coral_access_token_ = token;
        cached_session_token_ = session_token;
        user_id_ = json.string("userId");
        coral_token_expiry_ = expires_at;
        return true;
    } catch (...) {
        SecureStore::erase(kCoralCredentialAccount);
        return false;
    }
}

void CoralClient::persist_session(const std::string& session_token) {
    if (!SecureStore::available() || coral_access_token_.empty()) return;
    const Json json(Json::object{
        {"sessionHash", session_hash(session_token)},
        {"accessToken", coral_access_token_},
        {"userId", user_id_},
        {"expiresAt", epoch_seconds(coral_token_expiry_)},
    });
    SecureStore::put(kCoralCredentialAccount, json.dump());
}

void CoralClient::load_auth_attempts(
    const std::string& session_token,
    Clock::time_point now) {
    std::lock_guard rate_lock(rate_limit_mutex_);
    const auto hash = auth_rate_limit_key(session_token);
    if (rate_limit_session_hash_ == hash) {
        while (!auth_attempts_.empty() &&
               now - auth_attempts_.front() > std::chrono::hours(1)) {
            auth_attempts_.pop_front();
        }
        return;
    }

    rate_limit_session_hash_ = hash;
    auth_attempts_.clear();
    coral_rate_limit_until_ = {};
    const auto file = cache_directory_ / ("coral-rate-limit-" + hash + ".json");
    try {
        std::ifstream input(file, std::ios::binary);
        if (input) {
            const std::string contents{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            const auto json = Json::parse(contents);
            coral_rate_limit_until_ = time_from_epoch(
                json.integer("backoffUntil"));
            if (const auto* attempts = json.find("attempts");
                attempts != nullptr && attempts->is_array()) {
                for (const auto& item : attempts->as_array()) {
                    if (item.is_number()) {
                        const auto when = time_from_epoch(item.as_i64());
                        if (when <= now && now - when <= std::chrono::hours(1)) {
                            auth_attempts_.push_back(when);
                        }
                    }
                }
            }
        }
    } catch (...) {
        auth_attempts_.clear();
    }
}

void CoralClient::save_auth_attempts() const {
    std::lock_guard rate_lock(rate_limit_mutex_);
    if (rate_limit_session_hash_.empty()) return;
    try {
        std::filesystem::create_directories(cache_directory_);
        Json::array attempts;
        for (const auto& item : auth_attempts_) {
            attempts.emplace_back(epoch_seconds(item));
        }
        const Json json(Json::object{
            {"attempts", std::move(attempts)},
            {"backoffUntil", epoch_seconds(coral_rate_limit_until_)},
        });
        const auto file = cache_directory_ /
            ("coral-rate-limit-" + rate_limit_session_hash_ + ".json");
        auto temporary = file;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return;
            output << json.dump();
            output.flush();
            if (!output) return;
        }
#ifndef _WIN32
        chmod(temporary.c_str(), 0600);
#endif
        replace_cache_file(temporary, file);
#ifndef _WIN32
        chmod(file.c_str(), 0600);
#endif
    } catch (...) {
    }
}

void CoralClient::throw_if_coral_rate_limited(Clock::time_point now) const {
    std::lock_guard rate_lock(rate_limit_mutex_);
    if (now < coral_rate_limit_until_) {
        throw std::runtime_error("Nintendo Coral Retry-After backoff is active");
    }
}

void CoralClient::apply_coral_rate_limit_response(const HttpResponse& response) {
    if (response.status != 429) return;

    auto delay = std::chrono::duration_cast<std::chrono::seconds>(
        kDefaultCoralRateLimitBackoff);
    if (const auto header = response.headers.find("retry-after");
        header != response.headers.end()) {
        delay = retry_after_delay(header->second);
    }

    {
        std::lock_guard rate_lock(rate_limit_mutex_);
        coral_rate_limit_until_ = Clock::now() + delay;
        save_auth_attempts();
    }
}

void CoralClient::clear_cached_session() {
    // Invalidate in-flight authentication before touching the cache. A slow
    // Nintendo/nxapi request may still return, but it is forbidden from
    // repopulating a credential after the user has signed out.
    session_generation_.fetch_add(1);
    {
        std::lock_guard lock(session_mutex_);
        coral_access_token_.clear();
        cached_session_token_.clear();
        user_id_.clear();
        na_id_.clear();
        coral_token_expiry_ = {};
        web_service_tokens_.clear();
    }
    SecureStore::erase(kCoralCredentialAccount);
}

std::string CoralClient::ensure_session(const std::string& session_token) {
    // Only one Coral authentication may be in flight, but do not hold the
    // cache mutex across network I/O: sign-out and Exit must remain responsive.
    std::lock_guard login_lock(login_mutex_);
    const auto generation = session_generation_.load();
    const auto now = Clock::now();

    load_auth_attempts(session_token, now);
    throw_if_coral_rate_limited(now);

    {
        std::lock_guard cache_lock(session_mutex_);
        if (!coral_access_token_.empty() &&
            cached_session_token_ == session_token &&
            now < coral_token_expiry_) {
            return coral_access_token_;
        }

        if (restore_persistent_session(session_token, now)) {
            return coral_access_token_;
        }
    }

    {
        std::lock_guard rate_lock(rate_limit_mutex_);
        if (auth_attempts_.size() >= kMaxCoralAuthAttemptsPerHour) {
            throw std::runtime_error(
                "Coral authentication paused locally: four attempts were already made in the last hour");
        }
        auth_attempts_.push_back(now);
        save_auth_attempts();
    }

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
    apply_coral_rate_limit_response(response);

    if (response.status / 100 != 2) {
        std::string detail;
        if (!response.body.empty()) {
            try {
                detail = nxapi_.decrypt_response(response.body);
            } catch (...) {
            }
        }
        throw std::runtime_error(
            "Coral login failed (HTTP " + std::to_string(response.status) + ")" +
            (detail.empty() ? std::string{} : ": " + detail));
    }

    const auto decrypted_login = nxapi_.decrypt_response(response.body);
    const auto login = Json::parse(decrypted_login);
    const auto* result = login.find("result");
    if (result == nullptr) throw std::runtime_error("Coral login missing result");
    const auto* credential = result->find("webApiServerCredential");
    if (credential == nullptr) {
        throw std::runtime_error("Coral login missing credential");
    }

    const auto access_token = credential->string("accessToken");
    if (access_token.empty()) {
        throw std::runtime_error("Coral login missing access token");
    }
    // Coral user IDs are JSON numbers in Nintendo's API and can exceed the
    // exact-integer range of IEEE-754 doubles. Preserve the original decimal
    // text for the ShowSelf request instead of letting the generic JSON number
    // representation round a 64-bit ID.
    std::string user_id = exact_integer_field_in_object(
        decrypted_login, "user", "id");
    if (user_id.empty()) {
        if (const auto* user = result->find("user")) {
            user_id = user->string("id", user->string("nsaId"));
        }
    }

    // If sign-out happened while authentication was in flight, do not put the
    // just-returned credential back into memory or the OS secure store.
    if (session_generation_.load() != generation) {
        throw std::runtime_error("Coral authentication cancelled");
    }

    // Use the server-provided lifetime. 7200 seconds is commonly returned, but
    // it is deliberately not hard-coded because Nintendo can change it.
    const auto expires_in = credential->integer("expiresIn", 7200);
    {
        std::lock_guard cache_lock(session_mutex_);
        if (session_generation_.load() != generation) {
            throw std::runtime_error("Coral authentication cancelled");
        }
        coral_access_token_ = access_token;
        user_id_ = std::move(user_id);
        na_id_ = profile.id;
        cached_session_token_ = session_token;
        coral_token_expiry_ = Clock::now() + std::chrono::seconds(
            std::max<std::int64_t>(1, expires_in));
        persist_session(session_token);
    }
    return access_token;
}

Json CoralClient::coral_call(
    const std::string& url,
    const std::string& access_token,
    const std::string& request_body) {
    // Presence and album sync can wake at the same time. Serialize Coral calls
    // to prevent avoidable request bursts and keep nxapi encryption work ordered.
    std::lock_guard request_lock(request_mutex_);
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
    apply_coral_rate_limit_response(response);

    if (response.status / 100 != 2) {
        if (response.status == 401 || response.status == 403) {
            clear_cached_session();
        }
        std::string detail;
        if (!response.body.empty()) {
            try {
                detail = nxapi_.decrypt_response(response.body);
            } catch (...) {
            }
        }
        throw std::runtime_error(
            "Coral request failed (HTTP " + std::to_string(response.status) + ")" +
            (detail.empty() ? std::string{} : ": " + detail));
    }
    return Json::parse(nxapi_.decrypt_response(response.body));
}

std::vector<MediaItem> CoralClient::media_list(
    const std::string& session_token) {
    const auto access_token = ensure_session(session_token);
    const auto response = coral_call(
        coral_url(kMediaListPath), access_token, R"({"parameter":{}})");

    std::vector<MediaItem> media;
    const auto* result = response.find("result");
    if (result == nullptr) return media;
    const auto* media_json = result->find("media");
    if (media_json == nullptr || !media_json->is_array()) return media;

    media.reserve(media_json->as_array().size());
    for (const auto& item : media_json->as_array()) {
        media.push_back(parse_media_item(item));
    }
    return media;
}

NintendoPresence CoralClient::self_presence(
    const std::string& session_token) {
    const auto access_token = ensure_session(session_token);
    std::string user_id;
    {
        std::lock_guard lock(session_mutex_);
        user_id = user_id_;
    }
    const std::string request_body = user_id.empty()
        ? R"({"parameter":{}})"
        : R"({"parameter":{"id":)" + user_id + "}}";
    const auto response = coral_call(
        coral_url(kShowSelfPath), access_token, request_body);
    const auto* result = response.find("result");
    if (result == nullptr) return {};
    return parse_presence(*result);
}

std::string CoralClient::get_web_service_token(
    const std::string& session_token,
    std::uint64_t game_service_id) {
    const auto now = Clock::now();
    {
        std::lock_guard lock(session_mutex_);
        if (cached_session_token_ == session_token) {
            if (game_service_id != 0) {
                const auto it = web_service_tokens_.find(game_service_id);
                if (it != web_service_tokens_.end() &&
                    !it->second.token.empty() &&
                    now < it->second.expires_at) {
                    return it->second.token;
                }
            } else {
                // If game_service_id == 0, reuse any currently active valid token
                for (const auto& [id, cached] : web_service_tokens_) {
                    if (!cached.token.empty() && now < cached.expires_at) {
                        return cached.token;
                    }
                }
            }
        }
    }

    try {
        const auto access_token = ensure_session(session_token);
        std::string coral_user_id;
        std::string na_id;
        {
            std::lock_guard lock(session_mutex_);
            coral_user_id = user_id_;
            na_id = na_id_;
        }
        if (na_id.empty()) {
            const auto nintendo_tokens = auth_.exchange_session_token(session_token);
            const auto profile = auth_.fetch_profile(nintendo_tokens.access_token);
            na_id = profile.id;
            std::lock_guard lock(session_mutex_);
            na_id_ = na_id;
        }

        const std::uint64_t target_service_id = (game_service_id != 0)
            ? game_service_id
            : 4834290508791808ULL; // Universal fallback service ID

        std::lock_guard request_lock(request_mutex_);
        const auto nso_version = nxapi_.nso_version();

        std::vector<unsigned char> encrypted_request;
        try {
            encrypted_request = nxapi_.encrypted_web_service_token_body(
                access_token, na_id, coral_user_id, target_service_id);
        } catch (...) {
            const auto attestation = nxapi_.generate_f(2, access_token, na_id, coral_user_id);
            const Json parameter(Json::object{
                {"id", static_cast<double>(target_service_id)},
                {"registrationToken", ""},
                {"f", attestation.f},
                {"requestId", attestation.request_id},
                {"timestamp", static_cast<double>(attestation.timestamp)},
            });
            const Json request_body(Json::object{
                {"parameter", parameter},
            });
            encrypted_request = nxapi_.encrypt_request(
                coral_url("/v4/Game/GetWebServiceToken"),
                access_token,
                request_body.dump());
        }

        const auto response = http_.post_bytes(
            coral_url("/v4/Game/GetWebServiceToken"),
            encrypted_request,
            {
                "Content-Type: application/octet-stream",
                "Accept: application/octet-stream,application/json",
                "Accept-Language: en-US",
                "Authorization: Bearer " + access_token,
                "X-Platform: Android",
                "X-ProductVersion: " + nso_version,
                "User-Agent: com.nintendo.znca/" + nso_version + "(Android/12)",
            });
        apply_coral_rate_limit_response(response);

        if (response.status / 100 != 2) {
            if (response.status == 401 || response.status == 403) {
                clear_cached_session();
            }
            std::string detail;
            if (!response.body.empty()) {
                try {
                    detail = nxapi_.decrypt_response(response.body);
                } catch (...) {
                }
            }
            throw std::runtime_error(
                "Coral GetWebServiceToken failed (HTTP " +
                std::to_string(response.status) + ")" +
                (detail.empty() ? std::string{} : ": " + detail));
        }

        const auto decrypted = nxapi_.decrypt_response(response.body);
        const auto json = Json::parse(decrypted);
        const auto* result = json.find("result");
        if (result == nullptr) return {};

        const auto token = result->string("accessToken");
        if (token.empty()) return {};

        const auto expires_in = result->integer("expiresIn", 10800);
        const auto ttl = std::chrono::seconds(
            std::max<std::int64_t>(60, expires_in - 120));

        {
            std::lock_guard lock(session_mutex_);
            if (cached_session_token_ == session_token) {
                web_service_tokens_[target_service_id] = CachedWebServiceToken{token, now + ttl};
            }
        }

        return token;
    } catch (...) {
        return {};
    }
}

}  // namespace nso
