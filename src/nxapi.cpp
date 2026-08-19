#include "nso_album_sync/nxapi.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/secure_store.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#else
#include <sys/stat.h>
#endif

namespace nso {
namespace {

constexpr char kWorkerBaseUrl[] =
    "https://nso-worker-backend.diogoenes0.workers.dev";
constexpr char kNativeSessionStartPath[] = "/api/nso/native/session/start";
constexpr char kNativeSessionReleasePath[] = "/api/nso/native/session/release";
constexpr char kNativeNxapiConfigPath[] = "/api/nso/native/nxapi/config";
constexpr char kNativeNxapiFPath[] = "/api/nso/native/nxapi/f";
constexpr char kNativeNxapiEncryptPath[] = "/api/nso/native/nxapi/encrypt-request";
constexpr char kNativeNxapiDecryptPath[] = "/api/nso/native/nxapi/decrypt-response";
constexpr char kNintendoSecureStoreAccount[] = "NintendoAccount";
constexpr char kUserAgent[] =
    "nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)";
constexpr char kCoralLoginUrl[] =
    "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login";

constexpr auto kVersionCacheLifetime = std::chrono::hours(6);
constexpr auto kVersionRetryLifetime = std::chrono::minutes(15);
constexpr auto kDefaultRateLimitBackoff = std::chrono::minutes(15);
constexpr auto kBrokerSafetyMargin = std::chrono::seconds(30);

std::string worker_url(const char* path) {
    return std::string(kWorkerBaseUrl) + path;
}

std::int64_t epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point time_from_epoch(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

std::chrono::system_clock::time_point broker_expiry_from_json(const Json& json) {
    const auto expires_at = json.integer("expiresAt");
    if (expires_at > 100000000000LL) {
        return std::chrono::system_clock::time_point(
            std::chrono::milliseconds(expires_at));
    }
    if (expires_at > 1000000000LL) {
        return time_from_epoch(expires_at);
    }

    const auto expires_in = std::max<std::int64_t>(
        60, json.integer("expiresIn", 7200));
    return std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
}

std::string response_error(
    const HttpResponse& response,
    const std::string& label) {
    std::string message = label + " (HTTP " + std::to_string(response.status) + ")";
    try {
        const auto json = Json::parse(response.text());
        auto detail = json.string("error_description");
        if (detail.empty()) detail = json.string("error_message");
        if (detail.empty()) detail = json.string("error");
        if (!detail.empty()) message += ": " + detail;
    } catch (...) {
        // Never append an arbitrary upstream/body dump here. The Worker owns
        // confidential nxapi credentials and client errors must stay sanitized.
    }
    return message;
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
            kDefaultRateLimitBackoff);
    }

#ifdef _WIN32
    const std::time_t retry_at = _mkgmtime(&parsed);
#else
    const std::time_t retry_at = timegm(&parsed);
#endif
    if (retry_at <= 0) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultRateLimitBackoff);
    }

    const auto now = std::time(nullptr);
    return std::chrono::seconds(std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(retry_at - now)));
}

}  // namespace

NxapiClient::NxapiClient(
    HttpClient& http,
    std::string legacy_client_id,
    std::filesystem::path cache_file)
    : http_(http),
      cache_file_(std::move(cache_file)),
      native_client_id_("desktop_" + base64url(random_bytes(18))) {
    // Public nxapi-auth client IDs are deliberately ignored. Authentication is
    // now performed by the confidential Cloudflare Worker and no nxapi OAuth
    // bearer/refresh token is ever returned to this desktop process.
    (void)legacy_client_id;
    load_cache();
}

void NxapiClient::bind_nintendo_auth(NintendoAuthManager& auth) {
    std::lock_guard lock(auth_mutex_);
    nintendo_auth_ = &auth;
}

void NxapiClient::load_cache() {
    if (cache_file_.empty() || !std::filesystem::exists(cache_file_)) return;
    try {
        std::ifstream file(cache_file_, std::ios::binary);
        const std::string contents{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        const auto json = Json::parse(contents);
        version_ = json.string("nsoVersion");
        version_expiry_ = time_from_epoch(json.integer("nsoVersionExpiresAt"));
        rate_limit_until_ = time_from_epoch(json.integer("rateLimitUntil"));
    } catch (...) {
        version_.clear();
        version_expiry_ = {};
        rate_limit_until_ = {};
    }
}

void NxapiClient::save_cache() const {
    if (cache_file_.empty()) return;
    try {
        std::filesystem::create_directories(cache_file_.parent_path());
        const Json json(Json::object{
            {"nsoVersion", version_},
            {"nsoVersionExpiresAt", epoch_seconds(version_expiry_)},
            {"rateLimitUntil", epoch_seconds(rate_limit_until_)},
        });
        auto temporary = cache_file_;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) return;
            file << json.dump();
            file.flush();
            if (!file) return;
        }
#ifndef _WIN32
        chmod(temporary.c_str(), 0600);
#endif
        replace_cache_file(temporary, cache_file_);
#ifndef _WIN32
        chmod(cache_file_.c_str(), 0600);
#endif
    } catch (...) {
        // Cache persistence is an optimisation; never break login because disk
        // cache storage is unavailable. Only non-secret version/backoff state is
        // written here.
    }
}

std::string NxapiClient::nintendo_access_token() {
    NintendoAuthManager* auth = nullptr;
    {
        std::lock_guard lock(auth_mutex_);
        auth = nintendo_auth_;
    }
    if (auth == nullptr) {
        throw std::runtime_error(
            "Nintendo authentication is not bound to the NSO backend client");
    }

    auto access_token = auth->cached_access_token();
    if (!access_token.empty()) return access_token;

    // During a normal fresh sign-in the auth manager remembers the Nintendo
    // session token in memory. On a restored sign-in, retrieve the same token
    // from the platform secure store. It is never copied to nxapi-cache.json.
    auto session_token = auth->cached_session_token();
    if (session_token.empty()) {
        if (const auto stored = SecureStore::get(kNintendoSecureStoreAccount)) {
            session_token = *stored;
        }
    }
    if (session_token.empty()) {
        throw std::runtime_error(
            "Nintendo Account session is unavailable for NSO backend authentication");
    }

    const auto tokens = auth->exchange_session_token(session_token);
    if (tokens.access_token.empty()) {
        throw std::runtime_error(
            "Nintendo Account token exchange returned no access token");
    }
    return tokens.access_token;
}

std::string NxapiClient::ensure_broker_token() {
    std::lock_guard request_lock(auth_request_mutex_);
    const auto generation = auth_generation_.load();
    const auto now = Clock::now();

    {
        std::lock_guard state_lock(auth_mutex_);
        if (!broker_token_.empty() && now + kBrokerSafetyMargin < broker_expiry_) {
            return broker_token_;
        }
    }

    const auto access_token = nintendo_access_token();
    const Json payload(Json::object{
        {"nintendoAccessToken", access_token},
        {"clientId", native_client_id_},
    });

    const auto response = http_.post(
        worker_url(kNativeSessionStartPath),
        payload.dump(),
        {
            "Accept: application/json",
            "Cache-Control: no-store",
            std::string("User-Agent: ") + kUserAgent,
        },
        "application/json",
        20);

    if (response.status / 100 != 2) {
        if (response.status == 429) apply_rate_limit_response(response);
        throw std::runtime_error(response_error(
            response, "NSO backend native session start failed"));
    }

    const auto json = Json::parse(response.text());
    const auto broker_token = json.string("brokerToken");
    if (broker_token.empty()) {
        throw std::runtime_error(
            "NSO backend native session response is missing brokerToken");
    }
    const auto broker_expiry = broker_expiry_from_json(json);

    if (auth_generation_.load() != generation) {
        throw std::runtime_error("NSO backend authentication cancelled");
    }
    {
        std::lock_guard state_lock(auth_mutex_);
        if (auth_generation_.load() != generation) {
            throw std::runtime_error("NSO backend authentication cancelled");
        }
        broker_token_ = broker_token;
        broker_expiry_ = broker_expiry;
    }
    return broker_token;
}

void NxapiClient::clear_user_auth() {
    auth_generation_.fetch_add(1);

    std::string broker_token;
    {
        std::lock_guard lock(auth_mutex_);
        broker_token.swap(broker_token_);
        broker_expiry_ = {};
    }

    // Best-effort server cleanup. Local invalidation happens first, so sign-out
    // is complete even if the Worker is unreachable. The broker credential is
    // short-lived and remains account-bound server-side.
    if (!broker_token.empty()) {
        try {
            const Json payload(Json::object{{"clientId", native_client_id_}});
            (void)http_.post(
                worker_url(kNativeSessionReleasePath),
                payload.dump(),
                {
                    "Accept: application/json",
                    "Authorization: Bearer " + broker_token,
                    std::string("User-Agent: ") + kUserAgent,
                },
                "application/json",
                5);
        } catch (...) {
        }
    }
}

void NxapiClient::throw_if_rate_limited() const {
    if (Clock::now() < rate_limit_until_) {
        throw std::runtime_error(
            "nxapi-znca-api Retry-After backoff is active");
    }
}

void NxapiClient::apply_rate_limit_response(const HttpResponse& response) {
    if (response.status == 406) {
        // The cached Nintendo Switch Online version is explicitly unsupported.
        // Do not retry this request; force the next independent action to fetch
        // config instead of repeatedly sending a known-bad version.
        version_.clear();
        version_expiry_ = {};
        save_cache();
    }

    if (response.status != 429) return;

    auto delay = std::chrono::duration_cast<std::chrono::seconds>(
        kDefaultRateLimitBackoff);
    if (const auto header = response.headers.find("retry-after");
        header != response.headers.end()) {
        delay = retry_after_delay(header->second);
    }

    rate_limit_until_ = Clock::now() + delay;
    save_cache();
}

bool NxapiClient::is_broker_auth_failure(const HttpResponse& response) const {
    if (response.status != 401) return false;
    try {
        const auto error = Json::parse(response.text()).string("error");
        return error == "broker_session_missing" ||
               error == "native_broker_missing" ||
               error == "native_broker_invalid" ||
               error == "native_broker_expired" ||
               error == "native_session_missing" ||
               error == "native_session_expired" ||
               error == "invalid_native_broker";
    } catch (...) {
        return false;
    }
}

HttpResponse NxapiClient::worker_request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& accept,
    bool allow_broker_retry) {
    // Preserve the old client's one-at-a-time behavior. This is especially
    // important for /f because an ambiguous duplicate request can spend a
    // user's attestation allowance twice.
    std::lock_guard lock(request_mutex_);
    throw_if_rate_limited();

    const auto perform = [&](const std::string& broker_token) {
        std::vector<std::string> headers{
            "Accept: " + accept,
            "Authorization: Bearer " + broker_token,
            "X-NSO-Native-Client-Id: " + native_client_id_,
            std::string("User-Agent: ") + kUserAgent,
        };
        return http_.request(
            method,
            std::string(kWorkerBaseUrl) + path,
            headers,
            std::vector<unsigned char>(body.begin(), body.end()),
            body.empty() ? "" : "application/json",
            30);
    };

    auto broker_token = ensure_broker_token();
    auto response = perform(broker_token);

    // Retry only when the Worker explicitly rejected the native broker before
    // touching nxapi. Never retry an upstream /f failure or a generic 401.
    if (allow_broker_retry && is_broker_auth_failure(response)) {
        {
            std::lock_guard state_lock(auth_mutex_);
            if (broker_token_ == broker_token) {
                broker_token_.clear();
                broker_expiry_ = {};
            }
        }
        broker_token = ensure_broker_token();
        response = perform(broker_token);
    }

    apply_rate_limit_response(response);
    return response;
}

std::string NxapiClient::nso_version() {
    std::lock_guard lock(request_mutex_);
    const auto now = Clock::now();
    if (!version_.empty() && now < version_expiry_) return version_;

    const auto response = worker_request(
        "GET", kNativeNxapiConfigPath, "", "application/json");
    if (response.status / 100 == 2) {
        const auto json = Json::parse(response.text());
        auto supported_version = json.string("nso_version");
        if (supported_version.empty()) {
            if (const auto* config = json.find("config")) {
                supported_version = config->string("nso_version");
            }
        }
        if (!supported_version.empty()) {
            version_ = supported_version;
            version_expiry_ = now + kVersionCacheLifetime;
            save_cache();
            return version_;
        }
    }

    if (!version_.empty()) {
        version_expiry_ = now + kVersionRetryLifetime;
        save_cache();
        return version_;
    }
    throw std::runtime_error(response_error(
        response, "Could not retrieve supported NSO version from NSO backend"));
}

std::vector<unsigned char> NxapiClient::encrypted_login_body(
    const std::string& id_token,
    const UserProfile& profile) {
    const auto version = nso_version();

    // /f output is request-specific. The Worker owns nxapi OAuth credentials,
    // while only the operation result needed for Coral login returns here.
    const Json payload(Json::object{
        {"zncaVersion", version},
        {"token", id_token},
        {"hash_method", "1"},
        {"na_id", profile.id},
        {"encrypt_token_request",
         Json::object{
             {"url", kCoralLoginUrl},
             {"parameter",
              Json::object{
                  {"naIdToken", id_token},
                  {"naBirthday", profile.birthday},
                  {"naCountry", profile.country},
                  {"language", profile.language},
                  {"f", ""},
                  {"requestId", ""},
                  {"timestamp", 0},
              }},
         }},
    });

    const auto response = worker_request(
        "POST", kNativeNxapiFPath, payload.dump(), "application/json");
    if (response.status / 100 != 2) {
        throw std::runtime_error(response_error(
            response, "NSO backend nxapi /f failed"));
    }
    const auto encrypted =
        Json::parse(response.text()).string("encrypted_token_request");
    if (encrypted.empty()) {
        throw std::runtime_error(
            "NSO backend nxapi /f response is missing encrypted_token_request");
    }
    return base64_decode(encrypted);
}

FAttestation NxapiClient::generate_f(
    int hash_method,
    const std::string& token,
    const std::string& na_id,
    const std::string& coral_user_id) {
    const auto version = nso_version();
    const Json payload(Json::object{
        {"zncaVersion", version},
        {"hash_method", std::to_string(hash_method)},
        {"token", token},
        {"na_id", na_id},
        {"coral_user_id", coral_user_id},
    });

    const auto response = worker_request(
        "POST", kNativeNxapiFPath, payload.dump(), "application/json");
    if (response.status / 100 != 2) {
        throw std::runtime_error(response_error(
            response, "NSO backend nxapi /f failed"));
    }
    const auto json = Json::parse(response.text());
    FAttestation attestation;
    attestation.f = json.string("f");
    attestation.request_id = json.string("request_id");
    attestation.timestamp = json.integer("timestamp");
    if (attestation.f.empty() || attestation.request_id.empty() ||
        attestation.timestamp == 0) {
        throw std::runtime_error(
            "NSO backend nxapi /f returned incomplete attestation");
    }
    return attestation;
}

std::vector<unsigned char> NxapiClient::encrypted_web_service_token_body(
    const std::string& coral_access_token,
    const std::string& na_id,
    const std::string& coral_user_id,
    std::uint64_t game_service_id) {
    const auto version = nso_version();

    const Json payload(Json::object{
        {"zncaVersion", version},
        {"token", coral_access_token},
        {"hash_method", "2"},
        {"na_id", na_id},
        {"coral_user_id", coral_user_id},
        {"encrypt_token_request",
         Json::object{
             {"url", "https://api-lp1.znc.srv.nintendo.net/v4/Game/GetWebServiceToken"},
             {"parameter",
              Json::object{
                  {"id", static_cast<double>(game_service_id)},
                  {"registrationToken", ""},
                  {"f", ""},
                  {"requestId", ""},
                  {"timestamp", 0},
              }},
         }},
    });

    const auto response = worker_request(
        "POST", kNativeNxapiFPath, payload.dump(), "application/json");
    if (response.status / 100 != 2) {
        throw std::runtime_error(response_error(
            response, "NSO backend nxapi /f (hash_method 2) failed"));
    }
    const auto encrypted =
        Json::parse(response.text()).string("encrypted_token_request");
    if (encrypted.empty()) {
        throw std::runtime_error(
            "NSO backend nxapi /f (hash_method 2) response is missing encrypted_token_request");
    }
    return base64_decode(encrypted);
}

std::vector<unsigned char> NxapiClient::encrypt_request(
    const std::string& url,
    const std::string& coral_token,
    const std::string& json) {
    const auto version = nso_version();
    const Json payload(Json::object{
        {"zncaVersion", version},
        {"url", url},
        {"token", coral_token},
        {"data", json},
    });
    const auto response = worker_request(
        "POST", kNativeNxapiEncryptPath, payload.dump(), "application/json");
    if (response.status / 100 != 2) {
        throw std::runtime_error(response_error(
            response, "NSO backend nxapi /encrypt-request failed"));
    }
    const auto encrypted = Json::parse(response.text()).string("data");
    if (encrypted.empty()) {
        throw std::runtime_error(
            "NSO backend nxapi /encrypt-request response is missing data");
    }
    return base64_decode(encrypted);
}

std::string NxapiClient::decrypt_response(
    const std::vector<unsigned char>& body) {
    const auto version = nso_version();
    const Json payload(Json::object{
        {"zncaVersion", version},
        {"data", base64_encode(body)},
    });
    const auto response = worker_request(
        "POST", kNativeNxapiDecryptPath, payload.dump(), "text/plain");
    if (response.status / 100 != 2) {
        throw std::runtime_error(response_error(
            response, "NSO backend nxapi /decrypt-response failed"));
    }
    return response.text();
}

}  // namespace nso
