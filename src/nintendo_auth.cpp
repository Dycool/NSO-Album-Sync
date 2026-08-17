#include "nso_album_sync/nintendo_auth.hpp"

#include "nso_album_sync/json.hpp"
#include "nso_album_sync/util.hpp"

#include <stdexcept>

namespace nso {
namespace {

constexpr char kNintendoClientId[] = "71b963c1b7b6d119";
constexpr char kNintendoRedirectUri[] = "npf71b963c1b7b6d119://auth";
constexpr char kNintendoScope[] = "openid user user.birthday user.screenName";

constexpr char kAuthorizeUrl[] =
    "https://accounts.nintendo.com/connect/1.0.0/authorize";
constexpr char kSessionTokenUrl[] =
    "https://accounts.nintendo.com/connect/1.0.0/api/session_token";
constexpr char kTokenUrl[] =
    "https://accounts.nintendo.com/connect/1.0.0/api/token";
constexpr char kProfileUrl[] =
    "https://api.accounts.nintendo.com/2.0.0/users/me";

std::string extract_session_token_code(const std::string& input) {
    constexpr char marker[] = "session_token_code=";

    const auto marker_position = input.find(marker);
    if (marker_position == std::string::npos) {
        return input;
    }

    auto code = input.substr(marker_position + sizeof(marker) - 1);
    const auto parameter_end = code.find('&');
    if (parameter_end != std::string::npos) {
        code.resize(parameter_end);
    }

    return code;
}

}  // namespace

std::string NintendoAuthManager::authorize_url() {
    const auto state = base64url(random_bytes(36));
    pkce_verifier_ = base64url(random_bytes(32));
    const auto challenge = base64url(sha256(pkce_verifier_));

    return std::string(kAuthorizeUrl) +
           "?state=" + url_encode(state) +
           "&redirect_uri=" + url_encode(kNintendoRedirectUri) +
           "&client_id=" + kNintendoClientId +
           "&scope=" + url_encode(kNintendoScope) +
           "&response_type=session_token_code" +
           "&session_token_code_challenge=" + url_encode(challenge) +
           "&session_token_code_challenge_method=S256" +
           "&theme=login_form";
}

AuthResult NintendoAuthManager::complete_login(
    const std::string& redirect_url_or_code) {
    if (pkce_verifier_.empty()) {
        throw std::runtime_error(
            "PKCE verifier expired. Open the Nintendo sign-in page again.");
    }

    const auto code = extract_session_token_code(redirect_url_or_code);
    const auto session_token = exchange_code(code, pkce_verifier_);
    const auto tokens = exchange_session_token(session_token);

    std::string nickname = "Nintendo Switch Player";
    try {
        const auto profile = fetch_profile(tokens.access_token);
        if (!profile.nickname.empty()) {
            nickname = profile.nickname;
        }
    } catch (...) {
        // The session itself is still usable if the optional profile lookup fails.
    }

    return {
        session_token,
        tokens.id_token,
        tokens.access_token,
        nickname,
    };
}

std::string NintendoAuthManager::exchange_code(
    const std::string& session_token_code,
    const std::string& verifier) {
    const auto body = HttpClient::form_encode({
        {"client_id", kNintendoClientId},
        {"session_token_code", session_token_code},
        {"session_token_code_verifier", verifier},
    });

    const auto response = http_.post(
        kSessionTokenUrl,
        body,
        {"User-Agent: NASDKAPI; Android"},
        "application/x-www-form-urlencoded");

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Nintendo session-token exchange failed (HTTP " +
            std::to_string(response.status) + "): " +
            response.text());
    }

    const auto json = Json::parse(response.text());
    const auto session_token = json.string("session_token");

    if (session_token.empty()) {
        throw std::runtime_error("Nintendo response missing session_token");
    }

    return session_token;
}

TokenResponse NintendoAuthManager::exchange_session_token(
    const std::string& session_token) {
    const Json body(Json::object{
        {"client_id", kNintendoClientId},
        {"session_token", session_token},
        {"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer-session-token"},
    });

    const auto response = http_.post(
        kTokenUrl,
        body.dump(),
        {
            "Accept: application/json",
            "User-Agent: Dalvik/2.1.0 (Linux; U; Android 12)",
        });

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Nintendo token exchange failed (HTTP " +
            std::to_string(response.status) + "): " +
            response.text());
    }

    const auto json = Json::parse(response.text());
    return {
        json.string("id_token"),
        json.string("access_token"),
        static_cast<int>(json.integer("expires_in", 900)),
    };
}

UserProfile NintendoAuthManager::fetch_profile(
    const std::string& access_token) {
    const auto response = http_.get(
        kProfileUrl,
        {
            "Authorization: Bearer " + access_token,
            "Accept: application/json",
            "Accept-Language: en-GB",
            "User-Agent: NASDKAPI; Android",
        });

    if (response.status / 100 != 2) {
        throw std::runtime_error(
            "Nintendo profile request failed: " + response.text());
    }

    const auto json = Json::parse(response.text());
    return {
        json.string("id"),
        json.string("nickname"),
        json.string("birthday", "1995-01-01"),
        json.string("country", "US"),
        json.string("language", "en-GB"),
    };
}

}  // namespace nso
