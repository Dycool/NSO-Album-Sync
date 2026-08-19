#pragma once

#include "nso_album_sync/http.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace nso {

inline constexpr std::uint64_t kZeldaNotesGameServiceId = 5935781783175168ULL;
inline constexpr std::uint64_t kZeldaNotesGameServiceIdAlt = 4974384874151936ULL;

struct ZeldaNotesPresence {
    // Zelda Notes is a cookie-backed WebView service. Do not describe a map
    // position or activity unless the service itself returned a verifiable one.
    std::string title_name;
    std::string profile_summary;
    std::string stage_image_uri;
    bool active = false;

    std::string format_state() const { return title_name; }
    std::string format_details() const { return profile_summary; }
};

class ZeldaNotesClient {
public:
    explicit ZeldaNotesClient(HttpClient& http) : http_(http) {}

    ZeldaNotesPresence fetch_presence(const std::string& web_service_token);
    void clear_cache();

private:
    HttpClient& http_;
    std::mutex mutex_;
    std::string source_web_token_;
    std::string session_cookie_;
    std::chrono::system_clock::time_point session_expires_at_{};
};

}  // namespace nso
