#pragma once

#include "nso_album_sync/http.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace nso {

struct TokenResponse {
    std::string id_token;
    std::string access_token;
    int expires_in = 900;
};

struct UserProfile {
    std::string id;
    std::string nickname;
    std::string birthday = "1995-01-01";
    std::string country = "US";
    std::string language = "en-GB";
};

struct AuthResult {
    std::string session_token;
    std::string id_token;
    std::string access_token;
    std::string user_nickname;
};

class NintendoAuthManager {
public:
    explicit NintendoAuthManager(HttpClient& http) : http_(http) {}

    std::string authorize_url();
    AuthResult complete_login(const std::string& callback_url);
    TokenResponse exchange_session_token(const std::string& session_token);
    UserProfile fetch_profile(const std::string& access_token);
    void clear_cached_tokens();

    // The Worker broker only needs a short-lived Nintendo access token to bind
    // the desktop process to the signed-in Nintendo Account. Reuse the token
    // already obtained during normal sign-in whenever it is still safely valid.
    std::string cached_access_token() {
        std::lock_guard lock(token_cache_mutex_);
        if (cached_tokens_.access_token.empty()) return {};
        if (std::chrono::steady_clock::now() + std::chrono::seconds(10) >=
            cached_token_expiry_) {
            return {};
        }
        return cached_tokens_.access_token;
    }

    // Kept memory-only. This lets the Worker broker refresh a Nintendo access
    // token later in the same process even when the platform secure store is
    // unavailable and the app is running with a volatile sign-in.
    std::string cached_session_token() {
        std::lock_guard lock(token_cache_mutex_);
        return cached_session_token_;
    }

private:
    using Clock = std::chrono::steady_clock;

    HttpClient& http_;
    std::string pkce_verifier_;
    std::string oauth_state_;

    std::mutex token_cache_mutex_;
    std::string cached_session_token_;
    TokenResponse cached_tokens_;
    Clock::time_point cached_token_expiry_{};
    std::string cached_profile_access_token_;
    UserProfile cached_profile_;
    bool cached_profile_valid_ = false;

    std::string exchange_code(
        const std::string& session_token_code,
        const std::string& verifier);
};

}  // namespace nso
