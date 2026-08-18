#include "nso_album_sync/nintendo_auth.hpp"

#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/json.hpp"
#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace nso {
namespace {

constexpr char kNintendoClientId[] = "71b963c1b7b6d119";
constexpr char kNintendoRedirectUri[] = "npf71b963c1b7b6d119://auth";
constexpr char kNintendoScope[] = "openid user user.birthday user.screenName";
constexpr char kAuthorizeUrl[] = "https://accounts.nintendo.com/connect/1.0.0/authorize";
constexpr char kSessionTokenUrl[] = "https://accounts.nintendo.com/connect/1.0.0/api/session_token";
constexpr char kTokenUrl[] = "https://accounts.nintendo.com/connect/1.0.0/api/token";
constexpr char kProfileUrl[] = "https://api.accounts.nintendo.com/2.0.0/users/me";

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string url_decode_component(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

std::string extract_parameter(const std::string& input, const std::string& name) {
    const std::string marker = name + "=";
    std::size_t search_from = 0;
    std::size_t marker_position = std::string::npos;
    while ((marker_position = input.find(marker, search_from)) != std::string::npos) {
        if (marker_position == 0 || input[marker_position - 1] == '?' ||
            input[marker_position - 1] == '#' || input[marker_position - 1] == '&') {
            break;
        }
        search_from = marker_position + 1;
    }
    if (marker_position == std::string::npos) return {};
    auto value = input.substr(marker_position + marker.size());
    const auto parameter_end = value.find_first_of("&#");
    if (parameter_end != std::string::npos) value.resize(parameter_end);
    return url_decode_component(value);
}

std::string extract_session_token_code(const std::string& input) {
    const auto code = extract_parameter(input, "session_token_code");
    return code.empty() ? input : code;
}

}  // namespace

std::string NintendoAuthManager::authorize_url() {
    oauth_state_ = base64url(random_bytes(36));
    pkce_verifier_ = base64url(random_bytes(32));
    const auto challenge = base64url(sha256(pkce_verifier_));

    return std::string(kAuthorizeUrl) +
           "?state=" + url_encode(oauth_state_) +
           "&redirect_uri=" + url_encode(kNintendoRedirectUri) +
           "&client_id=" + kNintendoClientId +
           "&scope=" + url_encode(kNintendoScope) +
           "&response_type=session_token_code" +
           "&session_token_code_challenge=" + url_encode(challenge) +
           "&session_token_code_challenge_method=S256" +
           "&theme=login_form";
}

AuthResult NintendoAuthManager::complete_login(const std::string& redirect_url_or_code) {
    if (pkce_verifier_.empty() || oauth_state_.empty()) {
        throw std::runtime_error("Nintendo sign-in session expired. Open the sign-in page again.");
    }

    if (is_nintendo_auth_callback(redirect_url_or_code)) {
        const auto returned_state = extract_parameter(redirect_url_or_code, "state");
        if (returned_state.empty() || returned_state != oauth_state_) {
            throw std::runtime_error("Nintendo sign-in callback had an invalid OAuth state.");
        }

        const auto error = extract_parameter(redirect_url_or_code, "error");
        if (!error.empty()) {
            throw std::runtime_error(error == "access_denied"
                ? "Nintendo Account sign-in was cancelled."
                : "Nintendo Account sign-in failed: " + error);
        }

        if (extract_parameter(redirect_url_or_code, "session_token_code").empty()) {
            throw std::runtime_error("Nintendo sign-in callback did not include a session token code.");
        }
    }

    const auto code = extract_session_token_code(redirect_url_or_code);
    const auto session_token = exchange_code(code, pkce_verifier_);
    pkce_verifier_.clear();
    oauth_state_.clear();

    // Keep the short-lived Nintendo access/id token in memory. Coral login
    // immediately needs the same values, so exchanging the session token twice
    // after one sign-in is unnecessary traffic.
    const auto tokens = exchange_session_token(session_token);

    std::string nickname = "Nintendo Switch Player";
    try {
        const auto profile = fetch_profile(tokens.access_token);
        if (!profile.nickname.empty()) nickname = profile.nickname;
    } catch (...) {
    }

    return {session_token, tokens.id_token, tokens.access_token, nickname};
}

std::string NintendoAuthManager::exchange_code(
    const std::string& session_token_code,
    const std::string& verifier) {
    const auto body = HttpClient::form_encode({
        {"client_id", kNintendoClientId},
        {"session_token_code", session_token_code},
        {"session_token_code_verifier", verifier},
    });

    const auto response = http_.post(kSessionTokenUrl, body,
        {"User-Agent: NASDKAPI; Android"},
        "application/x-www-form-urlencoded");

    if (response.status / 100 != 2) {
        throw std::runtime_error("Nintendo session-token exchange failed (HTTP " +
            std::to_string(response.status) + "): " + response.text());
    }

    const auto json = Json::parse(response.text());
    const auto session_token = json.string("session_token");
    if (session_token.empty()) throw std::runtime_error("Nintendo response missing session_token");
    return session_token;
}

TokenResponse NintendoAuthManager::exchange_session_token(const std::string& session_token) {
    {
        std::lock_guard cache_lock(token_cache_mutex_);
        const auto now = Clock::now();
        if (session_token == cached_session_token_ &&
            !cached_tokens_.access_token.empty() && now < cached_token_expiry_) {
            return cached_tokens_;
        }
    }

    // Never hold the cache mutex while doing network I/O. Sign-out and explicit
    // exit must be able to clear local state even if Nintendo is slow/unreachable.
    const Json body(Json::object{
        {"client_id", kNintendoClientId},
        {"session_token", session_token},
        {"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer-session-token"},
    });

    const auto response = http_.post(kTokenUrl, body.dump(), {
        "Accept: application/json",
        "User-Agent: Dalvik/2.1.0 (Linux; U; Android 12)",
    });

    if (response.status / 100 != 2) {
        throw std::runtime_error("Nintendo token exchange failed (HTTP " +
            std::to_string(response.status) + "): " + response.text());
    }

    const auto json = Json::parse(response.text());
    TokenResponse tokens{
        json.string("id_token"),
        json.string("access_token"),
        static_cast<int>(json.integer("expires_in", 900)),
    };
    if (tokens.id_token.empty() || tokens.access_token.empty()) {
        throw std::runtime_error("Nintendo token exchange returned incomplete credentials");
    }

    {
        std::lock_guard cache_lock(token_cache_mutex_);
        cached_session_token_ = session_token;
        cached_tokens_ = tokens;
        const auto usable_seconds = std::max(1, tokens.expires_in - 10);
        cached_token_expiry_ = Clock::now() + std::chrono::seconds(usable_seconds);
        cached_profile_access_token_.clear();
        cached_profile_valid_ = false;
    }
    return tokens;
}

UserProfile NintendoAuthManager::fetch_profile(const std::string& access_token) {
    {
        std::lock_guard cache_lock(token_cache_mutex_);
        if (cached_profile_valid_ && cached_profile_access_token_ == access_token) {
            return cached_profile_;
        }
    }

    const auto response = http_.get(kProfileUrl, {
        "Authorization: Bearer " + access_token,
        "Accept: application/json",
        "Accept-Language: en-GB",
        "User-Agent: NASDKAPI; Android",
    });

    if (response.status / 100 != 2) {
        throw std::runtime_error("Nintendo profile request failed: " + response.text());
    }

    const auto json = Json::parse(response.text());
    UserProfile profile{
        json.string("id"),
        json.string("nickname"),
        json.string("birthday", "1995-01-01"),
        json.string("country", "US"),
        json.string("language", "en-GB"),
    };
    {
        std::lock_guard cache_lock(token_cache_mutex_);
        cached_profile_access_token_ = access_token;
        cached_profile_ = profile;
        cached_profile_valid_ = true;
    }
    return profile;
}

void NintendoAuthManager::clear_cached_tokens() {
    std::lock_guard cache_lock(token_cache_mutex_);
    cached_session_token_.clear();
    cached_tokens_ = {};
    cached_token_expiry_ = {};
    cached_profile_access_token_.clear();
    cached_profile_ = {};
    cached_profile_valid_ = false;
}

}  // namespace nso
