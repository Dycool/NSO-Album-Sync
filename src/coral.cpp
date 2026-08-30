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
#include <utility>

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
constexpr char kWorkerBaseUrl[] = "https://nso-worker-backend.diogoenes0.workers.dev";
constexpr char kWorkerClientId[] = "nso-album-sync";

// These limits apply only when the desktop client actually falls back to
// nxapi's f-generation endpoint. They deliberately live in process memory:
// Coral/Nintendo requests are not locally rate-limited or persisted.
constexpr std::size_t kNxapiMethod1MaxRequests = 10;
constexpr auto kNxapiMethod1Window = std::chrono::minutes(60);
constexpr std::size_t kNxapiMethod2MaxRequests = 20;
constexpr auto kNxapiMethod2Window = std::chrono::minutes(30);
constexpr auto kWorkerFCircuitBreakerDuration = std::chrono::hours(6);

std::mutex g_worker_f_health_mutex;
std::chrono::system_clock::time_point g_worker_f_method1_disabled_until{};
std::chrono::system_clock::time_point g_worker_f_method2_disabled_until{};

std::mutex g_nxapi_f_rate_limit_mutex;
std::unordered_map<std::string,
    std::deque<std::chrono::system_clock::time_point>> g_nxapi_f_method1_attempts;
std::unordered_map<std::string,
    std::deque<std::chrono::system_clock::time_point>> g_nxapi_f_method2_attempts;

bool worker_f_enabled(int hash_method) {
    std::lock_guard lock(g_worker_f_health_mutex);
    const auto now = std::chrono::system_clock::now();
    if (hash_method == 1) return now >= g_worker_f_method1_disabled_until;
    if (hash_method == 2) return now >= g_worker_f_method2_disabled_until;
    return false;
}

void disable_worker_f(int hash_method) {
    std::lock_guard lock(g_worker_f_health_mutex);
    const auto until = std::chrono::system_clock::now() +
        kWorkerFCircuitBreakerDuration;
    if (hash_method == 1) {
        g_worker_f_method1_disabled_until = until;
    } else if (hash_method == 2) {
        g_worker_f_method2_disabled_until = until;
    }
}

FAttestation nxapi_generate_f_limited(
    NxapiClient& nxapi,
    int hash_method,
    const std::string& token,
    const std::string& na_id,
    const std::string& coral_user_id) {
    const auto now = std::chrono::system_clock::now();
    const auto fallback_key = base64url(sha256(token));

    {
        std::lock_guard lock(g_nxapi_f_rate_limit_mutex);
        if (hash_method == 1) {
            const auto key = na_id.empty() ? fallback_key : na_id;
            auto& attempts = g_nxapi_f_method1_attempts[key];
            while (!attempts.empty() &&
                   now - attempts.front() >= kNxapiMethod1Window) {
                attempts.pop_front();
            }
            if (attempts.size() >= kNxapiMethod1MaxRequests) {
                throw std::runtime_error(
                    "nxapi method-1 f-token limit reached: 10 requests in 60 minutes");
            }
            attempts.push_back(now);
        } else if (hash_method == 2) {
            const auto key = coral_user_id.empty() ? fallback_key : coral_user_id;
            auto& attempts = g_nxapi_f_method2_attempts[key];
            while (!attempts.empty() &&
                   now - attempts.front() >= kNxapiMethod2Window) {
                attempts.pop_front();
            }
            if (attempts.size() >= kNxapiMethod2MaxRequests) {
                throw std::runtime_error(
                    "nxapi method-2 f-token limit reached: 20 requests in 30 minutes");
            }
            attempts.push_back(now);
        } else {
            throw std::invalid_argument("Unsupported nxapi f hash method");
        }
    }

    return nxapi.generate_f(hash_method, token, na_id, coral_user_id);
}

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

std::string worker_url(const char* path) {
    std::string url(kWorkerBaseUrl);
    url.append(path);
    return url;
}

FAttestation worker_generate_f(
    HttpClient& http,
    int hash_method,
    const std::string& token,
    const std::string& na_id,
    const std::string& coral_user_id) {
    const Json body(Json::object{
        {"clientId", kWorkerClientId},
        {"hashMethod", std::to_string(hash_method)},
        {"token", token},
        {"naId", na_id},
        {"coralUserId", coral_user_id},
    });
    const auto response = http.post(
        worker_url("/api/nso/f"),
        body.dump(),
        {"Accept: application/json", "User-Agent: nso-album-sync/2.0.0"});
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Native f-token generation failed (HTTP " +
            std::to_string(response.status) + "): " + response.text());
    }

    const auto json = Json::parse(response.text());
    FAttestation attestation;
    attestation.f = json.string("f");
    attestation.request_id = json.string("requestId", json.string("request_id"));
    attestation.timestamp = json.integer("timestamp");
    if (attestation.f.empty() || attestation.request_id.empty() ||
        attestation.timestamp == 0) {
        throw std::runtime_error("Native f-token response was incomplete");
    }
    return attestation;
}

bool is_f_rejection_payload(const std::string& payload) {
    if (payload.empty()) return false;
    try {
        const auto json = Json::parse(payload);
        const auto status = json.integer("status");
        if (status == 9403 || status == 9599) return true;
        const auto message = json.string("errorMessage");
        return message == "Invalid token." || message == "Unexpected error.";
    } catch (...) {
        return false;
    }
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
    return std::chrono::minutes(15);
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
        presence.title_id = game->string("titleId", game->string("applicationId"));

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
    (void)session_token;
    (void)now;
}

void CoralClient::save_auth_attempts() const {
}

void CoralClient::throw_if_coral_rate_limited(Clock::time_point now) const {
    (void)now;
}

void CoralClient::apply_coral_rate_limit_response(const HttpResponse& response) {
    (void)response;
}

void CoralClient::clear_cached_session() {
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
    std::lock_guard login_lock(login_mutex_);
    const auto generation = session_generation_.load();
    const auto now = Clock::now();

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

    const auto nintendo_tokens = auth_.exchange_session_token(session_token);
    const auto profile = auth_.fetch_profile(nintendo_tokens.access_token);

    bool used_worker_f = false;
    FAttestation attestation;
    if (worker_f_enabled(1)) {
        try {
            attestation = worker_generate_f(
                http_, 1, nintendo_tokens.id_token, profile.id, "");
            used_worker_f = true;
        } catch (...) {
            attestation = nxapi_generate_f_limited(
                nxapi_, 1, nintendo_tokens.id_token, profile.id, "");
        }
    } else {
        attestation = nxapi_generate_f_limited(
            nxapi_, 1, nintendo_tokens.id_token, profile.id, "");
    }

    const auto nso_version = nxapi_.nso_version();
    const auto perform_login_request =
        [&](const FAttestation& candidate)
            -> std::pair<HttpResponse, std::string> {
        const Json parameter(Json::object{
            {"naIdToken", nintendo_tokens.id_token},
            {"naBirthday", profile.birthday},
            {"naCountry", profile.country},
            {"language", profile.language},
            {"f", candidate.f},
            {"requestId", candidate.request_id},
            {"timestamp", static_cast<double>(candidate.timestamp)},
        });
        const Json request_body(Json::object{{"parameter", parameter}});
        const auto encrypted_login = nxapi_.encrypt_request(
            coral_url(kLoginPath), "", request_body.dump());

        auto response = http_.post_bytes(
            coral_url(kLoginPath),
            encrypted_login,
            {
                "X-Platform: Android",
                "X-ProductVersion: " + nso_version,
                "User-Agent: com.nintendo.znca/" + nso_version + "(Android/12)",
            });

        std::string decoded;
        if (!response.body.empty()) {
            try {
                decoded = nxapi_.decrypt_response(response.body);
            } catch (...) {
                const auto raw = response.text();
                const auto first = raw.find_first_not_of(" \t\r\n");
                if (first != std::string::npos &&
                    (raw[first] == '{' || raw[first] == '[')) {
                    decoded = raw;
                } else if (response.status / 100 == 2) {
                    throw;
                }
            }
        }
        return {std::move(response), std::move(decoded)};
    };

    auto login_attempt = perform_login_request(attestation);
    bool recovered_from_worker_f = false;
    if (used_worker_f && is_f_rejection_payload(login_attempt.second)) {
        const auto nxapi_attestation = nxapi_generate_f_limited(
            nxapi_, 1, nintendo_tokens.id_token, profile.id, "");
        login_attempt = perform_login_request(nxapi_attestation);
        recovered_from_worker_f = true;
    }

    const auto& response = login_attempt.first;
    const auto& decrypted_login = login_attempt.second;
    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Coral login failed (HTTP " + std::to_string(response.status) + ")" +
            (decrypted_login.empty() ? std::string{} : ": " + decrypted_login));
    }
    if (decrypted_login.empty()) {
        throw std::runtime_error("Coral login returned an empty response");
    }

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
    if (recovered_from_worker_f) {
        disable_worker_f(1);
    }
    std::string user_id = exact_integer_field_in_object(decrypted_login, "user", "id");
    if (user_id.empty()) {
        if (const auto* user = result->find("user")) {
            user_id = user->string("id", user->string("nsaId"));
        }
    }

    if (session_generation_.load() != generation) {
        throw std::runtime_error("Coral authentication cancelled");
    }

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
            for (const auto& [id, cached] : web_service_tokens_) {
                (void)id;
                if (!cached.token.empty() && now < cached.expires_at) {
                    return cached.token;
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
            : 4834290508791808ULL;

        std::lock_guard request_lock(request_mutex_);

        bool used_worker_f = false;
        FAttestation attestation;
        if (worker_f_enabled(2)) {
            try {
                attestation = worker_generate_f(
                    http_, 2, access_token, na_id, coral_user_id);
                used_worker_f = true;
            } catch (...) {
                attestation = nxapi_generate_f_limited(
                    nxapi_, 2, access_token, na_id, coral_user_id);
            }
        } else {
            attestation = nxapi_generate_f_limited(
                nxapi_, 2, access_token, na_id, coral_user_id);
        }

        const auto nso_version = nxapi_.nso_version();
        const auto perform_web_service_token_request =
            [&](const FAttestation& candidate)
                -> std::pair<HttpResponse, std::string> {
            const Json parameter(Json::object{
                {"id", static_cast<double>(target_service_id)},
                {"registrationToken", ""},
                {"f", candidate.f},
                {"requestId", candidate.request_id},
                {"timestamp", static_cast<double>(candidate.timestamp)},
            });
            const Json request_body(Json::object{{"parameter", parameter}});
            const auto encrypted_request = nxapi_.encrypt_request(
                coral_url("/v4/Game/GetWebServiceToken"),
                access_token,
                request_body.dump());

            auto response = http_.post_bytes(
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

            std::string decoded;
            if (!response.body.empty()) {
                try {
                    decoded = nxapi_.decrypt_response(response.body);
                } catch (...) {
                    const auto raw = response.text();
                    const auto first = raw.find_first_not_of(" \t\r\n");
                    if (first != std::string::npos &&
                        (raw[first] == '{' || raw[first] == '[')) {
                        decoded = raw;
                    } else if (response.status / 100 == 2) {
                        throw;
                    }
                }
            }
            return {std::move(response), std::move(decoded)};
        };

        auto token_attempt = perform_web_service_token_request(attestation);
        bool recovered_from_worker_f = false;
        if (used_worker_f && is_f_rejection_payload(token_attempt.second)) {
            const auto nxapi_attestation = nxapi_generate_f_limited(
                nxapi_, 2, access_token, na_id, coral_user_id);
            token_attempt = perform_web_service_token_request(nxapi_attestation);
            recovered_from_worker_f = true;
        }

        const auto& response = token_attempt.first;
        const auto& decrypted = token_attempt.second;
        if (response.status / 100 != 2) {
            if (response.status == 401 || response.status == 403) {
                clear_cached_session();
            }
            throw std::runtime_error(
                "Coral GetWebServiceToken failed (HTTP " +
                std::to_string(response.status) + ")" +
                (decrypted.empty() ? std::string{} : ": " + decrypted));
        }
        if (decrypted.empty()) return {};

        const auto json = Json::parse(decrypted);
        const auto* result = json.find("result");
        if (result == nullptr) return {};

        const auto token = result->string("accessToken");
        if (token.empty()) return {};

        if (recovered_from_worker_f) {
            disable_worker_f(2);
        }

        const auto expires_in = result->integer("expiresIn", 10800);
        const auto ttl = std::chrono::seconds(
            std::max<std::int64_t>(60, expires_in - 120));

        {
            std::lock_guard lock(session_mutex_);
            if (cached_session_token_ == session_token) {
                web_service_tokens_[target_service_id] =
                    CachedWebServiceToken{token, Clock::now() + ttl};
            }
        }

        return token;
    } catch (...) {
        return {};
    }
}

}  // namespace nso
