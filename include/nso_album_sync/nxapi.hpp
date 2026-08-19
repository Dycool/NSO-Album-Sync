#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/nintendo_auth.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace nso {

struct FAttestation {
    std::string f;
    std::string request_id;
    std::int64_t timestamp = 0;
};

class NxapiClient {
public:
    // The second argument is retained only so older App construction code and
    // config files remain source-compatible during the migration. It is ignored:
    // NSO Album Sync no longer authenticates directly to nxapi-auth.
    NxapiClient(
        HttpClient& http,
        std::string legacy_client_id,
        std::filesystem::path cache_file);

    void bind_nintendo_auth(NintendoAuthManager& auth);

    std::string nso_version();

    FAttestation generate_f(
        int hash_method,
        const std::string& token,
        const std::string& na_id,
        const std::string& coral_user_id);

    std::vector<unsigned char> encrypted_login_body(
        const std::string& id_token,
        const UserProfile& profile);

    std::vector<unsigned char> encrypted_web_service_token_body(
        const std::string& coral_access_token,
        const std::string& na_id,
        const std::string& coral_user_id,
        std::uint64_t game_service_id);

    std::vector<unsigned char> encrypt_request(
        const std::string& url,
        const std::string& coral_token,
        const std::string& json);

    std::string decrypt_response(const std::vector<unsigned char>& body);

    // Clears the short-lived native Worker broker credential. nxapi OAuth
    // access/refresh tokens never exist in this process anymore.
    void clear_user_auth();

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    NintendoAuthManager* nintendo_auth_ = nullptr;
    std::filesystem::path cache_file_;

    // All nxapi operations stay serialized, preserving the existing request
    // ordering and avoiding duplicate attestation work.
    std::recursive_mutex request_mutex_;
    std::mutex auth_mutex_;
    std::mutex auth_request_mutex_;
    std::atomic<std::uint64_t> auth_generation_{0};

    std::string native_client_id_;
    std::string broker_token_;
    Clock::time_point broker_expiry_{};

    std::string version_;
    Clock::time_point version_expiry_{};
    Clock::time_point rate_limit_until_{};

    std::string nintendo_access_token();
    std::string ensure_broker_token();
    bool is_broker_auth_failure(const HttpResponse& response) const;

    void throw_if_rate_limited() const;
    void apply_rate_limit_response(const HttpResponse& response);
    void load_cache();
    void save_cache() const;

    HttpResponse worker_request(
        const std::string& method,
        const std::string& path,
        const std::string& body,
        const std::string& accept,
        bool allow_broker_retry = true);
};

}  // namespace nso
