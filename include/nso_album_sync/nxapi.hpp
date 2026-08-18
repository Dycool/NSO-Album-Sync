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

class NxapiClient {
public:
    NxapiClient(
        HttpClient& http,
        std::string client_id,
        std::filesystem::path cache_file);

    std::string nso_version();

    std::vector<unsigned char> encrypted_login_body(
        const std::string& id_token,
        const UserProfile& profile);

    std::vector<unsigned char> encrypt_request(
        const std::string& url,
        const std::string& coral_token,
        const std::string& json);

    std::string decrypt_response(const std::vector<unsigned char>& body);

    // nxapi-auth access/refresh tokens are intentionally memory-only. The
    // public API terms say these credentials should not be persisted and may
    // only be used by a single Coral user.
    void clear_user_auth();

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    std::string client_id_;
    std::filesystem::path cache_file_;

    std::recursive_mutex request_mutex_;
    std::mutex auth_mutex_;
    std::mutex auth_request_mutex_;
    std::atomic<std::uint64_t> auth_generation_{0};

    std::string version_;
    std::string auth_token_;
    std::string refresh_token_;

    Clock::time_point version_expiry_{};
    Clock::time_point auth_expiry_{};
    Clock::time_point rate_limit_until_{};

    std::string auth_token();
    void throw_if_rate_limited() const;
    void apply_rate_limit_response(const HttpResponse& response);
    void load_cache();
    void save_cache() const;

    HttpResponse znca_request(
        const std::string& method,
        const std::string& path,
        const std::string& body,
        const std::string& accept,
        const std::vector<std::string>& extra_headers = {});
};

}  // namespace nso
