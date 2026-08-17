#include "nso_album_sync/nxapi.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace nso {
namespace {

constexpr char kZncaBaseUrl[] =
    "https://nxapi-znca-api.fancy.org.uk/api/znca";
constexpr char kNxapiAuthUrl[] =
    "https://nxapi-auth.fancy.org.uk/api/oauth/token";
constexpr char kUserAgent[] =
    "nso-album-sync/2.0 (+https://github.com/Dycool/NSO-Album-Sync)";
constexpr char kZncaClientVersion[] = "w8zSLBsxR7rVoGJA";
constexpr char kCoralLoginUrl[] =
    "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login";

constexpr auto kVersionCacheLifetime = std::chrono::hours(6);
constexpr auto kVersionRetryLifetime = std::chrono::minutes(15);
constexpr auto kDefaultRateLimitBackoff = std::chrono::minutes(15);

std::vector<std::string> znca_headers(const std::string& nso_version) {
    return {
        "X-znca-Platform: Android",
        "X-znca-Version: " + nso_version,
        std::string("X-znca-Client-Version: ") + kZncaClientVersion,
    };
}

}  // namespace

void NxapiClient::throw_if_rate_limited() const {
    if (Clock::now() < rate_limit_until_) {
        throw std::runtime_error(
            "nxapi-znca-api Retry-After backoff is active");
    }
}

void NxapiClient::apply_rate_limit_response(const HttpResponse& response) {
    if (response.status == 401) {
        // The nxapi-auth bearer token was rejected. Forget it and let the next
        // independent operation obtain a fresh token; do not retry this request.
        auth_token_.clear();
        auth_expiry_ = {};
    }

    if (response.status != 429) {
        return;
    }

    auto retry_after_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            kDefaultRateLimitBackoff)
            .count();

    const auto header = response.headers.find("retry-after");
    if (header != response.headers.end()) {
        try {
            retry_after_seconds = std::max<decltype(retry_after_seconds)>(
                1,
                std::stoll(header->second));
        } catch (...) {
            // Keep the conservative default when Retry-After is malformed.
        }
    }

    rate_limit_until_ =
        Clock::now() + std::chrono::seconds(retry_after_seconds);
}

std::string NxapiClient::auth_token() {
    std::lock_guard lock(auth_mutex_);
    const auto now = Clock::now();

    if (!auth_token_.empty() && now < auth_expiry_) {
        return auth_token_;
    }

    const auto request_token = [&](const std::string& grant_type,
                                   const std::string& refresh_token) {
        std::map<std::string, std::string> fields{
            {"grant_type", grant_type},
            {"client_id", client_id_},
            {"scope", "ca:gf ca:er ca:dr"},
        };

        if (!refresh_token.empty()) {
            fields["refresh_token"] = refresh_token;
        }

        return http_.post(
            kNxapiAuthUrl,
            HttpClient::form_encode(fields),
            {
                "Accept: application/json",
                std::string("User-Agent: ") + kUserAgent,
            },
            "application/x-www-form-urlencoded");
    };

    HttpResponse response;

    if (!refresh_token_.empty()) {
        response = request_token("refresh_token", refresh_token_);

        if (response.status / 100 != 2) {
            std::string error_code;
            try {
                error_code = Json::parse(response.text()).string("error");
            } catch (...) {
                // A non-JSON error is still reported below.
            }

            if (error_code != "invalid_grant") {
                throw std::runtime_error(
                    "nxapi-auth refresh failed: " + response.text());
            }

            refresh_token_.clear();
        }
    }

    if (refresh_token_.empty() || response.status / 100 != 2) {
        response = request_token("client_credentials", "");
    }

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi-auth token request failed: " + response.text());
    }

    const auto json = Json::parse(response.text());
    auth_token_ = json.string("access_token");
    refresh_token_ = json.string("refresh_token", refresh_token_);

    const auto expires_in = json.integer("expires_in", 300);
    auth_expiry_ =
        now + std::chrono::seconds(std::max<std::int64_t>(1, expires_in - 10));

    return auth_token_;
}

HttpResponse NxapiClient::znca_request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& accept,
    const std::vector<std::string>& extra_headers) {
    // Exactly one automated request may be in flight at a time.
    std::lock_guard lock(request_mutex_);
    throw_if_rate_limited();

    std::vector<std::string> headers{
        "Accept: " + accept,
        std::string("User-Agent: ") + kUserAgent,
    };

    const auto bearer_token = auth_token();
    if (!bearer_token.empty()) {
        headers.push_back("Authorization: Bearer " + bearer_token);
    }

    headers.insert(
        headers.end(),
        extra_headers.begin(),
        extra_headers.end());

    const auto response = http_.request(
        method,
        std::string(kZncaBaseUrl) + path,
        headers,
        std::vector<unsigned char>(body.begin(), body.end()),
        body.empty() ? "" : "application/json",
        30);

    apply_rate_limit_response(response);
    return response;
}

std::string NxapiClient::nso_version() {
    const auto now = Clock::now();

    if (!version_.empty() && now < version_expiry_) {
        return version_;
    }

    const auto response = znca_request(
        "GET",
        "/config",
        "",
        "application/json");

    if (response.status / 100 == 2) {
        const auto json = Json::parse(response.text());
        const auto supported_version = json.string("nso_version");

        if (!supported_version.empty()) {
            version_ = supported_version;
            version_expiry_ = now + kVersionCacheLifetime;
            return version_;
        }
    }

    // If /config temporarily fails, an already-known version is safer than
    // forcing repeated config requests.
    if (!version_.empty()) {
        version_expiry_ = now + kVersionRetryLifetime;
        return version_;
    }

    throw std::runtime_error(
        "Could not retrieve supported NSO version from nxapi /config");
}

std::vector<unsigned char> NxapiClient::encrypted_login_body(
    const std::string& id_token,
    const UserProfile& profile) {
    const auto version = nso_version();

    const Json payload(Json::object{
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

    const auto response = znca_request(
        "POST",
        "/f",
        payload.dump(),
        "application/json",
        znca_headers(version));

    if (response.status / 100 != 2) {
        throw std::runtime_error("nxapi /f failed: " + response.text());
    }

    const auto encrypted =
        Json::parse(response.text()).string("encrypted_token_request");

    if (encrypted.empty()) {
        throw std::runtime_error(
            "nxapi /f missing encrypted_token_request");
    }

    return base64_decode(encrypted);
}

std::vector<unsigned char> NxapiClient::encrypt_request(
    const std::string& url,
    const std::string& coral_token,
    const std::string& json) {
    const auto version = nso_version();

    const Json payload(Json::object{
        {"url", url},
        {"token", coral_token},
        {"data", json},
    });

    const auto response = znca_request(
        "POST",
        "/encrypt-request",
        payload.dump(),
        "application/json",
        znca_headers(version));

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi /encrypt-request failed: " + response.text());
    }

    const auto encrypted = Json::parse(response.text()).string("data");
    if (encrypted.empty()) {
        throw std::runtime_error(
            "nxapi /encrypt-request missing data");
    }

    return base64_decode(encrypted);
}

std::string NxapiClient::decrypt_response(
    const std::vector<unsigned char>& body) {
    const auto version = nso_version();

    const Json payload(Json::object{
        {"data", base64_encode(body)},
    });

    const auto response = znca_request(
        "POST",
        "/decrypt-response",
        payload.dump(),
        "text/plain",
        znca_headers(version));

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "nxapi /decrypt-response failed: " + response.text());
    }

    return response.text();
}

}  // namespace nso
