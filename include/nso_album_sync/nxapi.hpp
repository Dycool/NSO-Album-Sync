#pragma once

#include "nso_album_sync/http.hpp"
#include "nso_album_sync/nintendo_auth.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace nso {

class NxapiClient {
public:
    NxapiClient(HttpClient& http, std::string client_id)
        : http_(http), client_id_(std::move(client_id)) {}

    std::string nso_version();

    std::vector<unsigned char> encrypted_login_body(
        const std::string& id_token,
        const UserProfile& profile);

    std::vector<unsigned char> encrypt_request(
        const std::string& url,
        const std::string& coral_token,
        const std::string& json);

    std::string decrypt_response(const std::vector<unsigned char>& body);

private:
    using Clock = std::chrono::system_clock;

    HttpClient& http_;
    std::string client_id_;

    // nxapi's automated-use terms require serialized API requests. A recursive
    // mutex is used because request helpers may need the cached /config value.
    std::recursive_mutex request_mutex_;
    std::recursive_mutex auth_mutex_;

    std::string version_;
    std::string auth_token_;
    std::string refresh_token_;

    Clock::time_point version_expiry_{};
    Clock::time_point auth_expiry_{};
    Clock::time_point rate_limit_until_{};

    std::string auth_token();
    void throw_if_rate_limited() const;
    void apply_rate_limit_response(const HttpResponse& response);

    HttpResponse znca_request(
        const std::string& method,
        const std::string& path,
        const std::string& body,
        const std::string& accept,
        const std::vector<std::string>& extra_headers = {});
};

}  // namespace nso
