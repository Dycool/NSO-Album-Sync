#include "nso_album_sync/zeldanotes.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr char kBaseUrl[] = "https://api.lp1.87abc152.srv.nintendo.net";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Linux; Android 8.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.125 Mobile Safari/537.36";
constexpr char kLanguage[] = "en-GB";
constexpr char kCountry[] = "GB";
constexpr auto kSessionTtl = std::chrono::minutes(90);

std::string header_value(const HttpResponse& response, const std::string& key) {
    const auto it = response.headers.find(key);
    return it == response.headers.end() ? std::string{} : it->second;
}

std::string session_cookie(const HttpResponse& response) {
    const auto cookies = header_value(response, "set-cookie");
    if (cookies.empty()) return {};
    std::size_t start = 0;
    while (start < cookies.size()) {
        while (start < cookies.size() && (cookies[start] == ' ' || cookies[start] == ',' || cookies[start] == '\r' || cookies[start] == '\n')) ++start;
        const auto eq = cookies.find('=', start);
        if (eq == std::string::npos) break;
        auto name = cookies.substr(start, eq - start);
        auto lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "a5_token" || lower.find("session") != std::string::npos) {
            auto end = cookies.find_first_of(";,\r\n", eq + 1);
            if (end == std::string::npos) end = cookies.size();
            return name + "=" + cookies.substr(eq + 1, end - eq - 1);
        }
        const auto next = cookies.find(',', eq + 1);
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return {};
}

std::vector<std::string> bootstrap_headers(const std::string& token) {
    return {
        "Upgrade-Insecure-Requests: 1",
        std::string("User-Agent: ") + kUserAgent,
        "x-appplatform: android",
        "x-appcolorscheme: DARK",
        "x-gamewebtoken: " + token,
        "dnt: 1",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        std::string("Accept-Language: ") + kLanguage,
        "X-Requested-With: com.nintendo.znca",
    };
}

}  // namespace

void ZeldaNotesClient::clear_cache() {
    std::lock_guard lock(mutex_);
    source_web_token_.clear();
    session_cookie_.clear();
    session_expires_at_ = {};
}

ZeldaNotesPresence ZeldaNotesClient::fetch_presence(const std::string& web_service_token) {
    if (web_service_token.empty()) return {};

    try {
        std::string cookie;
        {
            std::lock_guard lock(mutex_);
            if (source_web_token_ == web_service_token && !session_cookie_.empty() &&
                std::chrono::system_clock::now() < session_expires_at_) cookie = session_cookie_;
        }

        if (cookie.empty()) {
            const auto bootstrap = http_.get(
                std::string(kBaseUrl) + "/?lang=" + kLanguage + "&na_country=" + kCountry + "&na_lang=" + kLanguage,
                bootstrap_headers(web_service_token), 10, 8 * 1024 * 1024);
            if (bootstrap.status / 100 != 2 && bootstrap.status / 100 != 3) return {};
            cookie = session_cookie(bootstrap);
            if (cookie.empty()) return {};
            std::lock_guard lock(mutex_);
            source_web_token_ = web_service_token;
            session_cookie_ = cookie;
            session_expires_at_ = std::chrono::system_clock::now() + kSessionTtl;
        }

        // Match the working backend: after the first GWT-authenticated document,
        // navigate with Nintendo's session cookie only. Do not keep injecting the
        // GameWebServiceToken into ordinary Zelda Notes requests.
        const auto page = http_.get(
            std::string(kBaseUrl) + "/title-select",
            {
                std::string("User-Agent: ") + kUserAgent,
                "Cookie: " + cookie,
                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
                std::string("Accept-Language: ") + kLanguage,
                "X-Requested-With: com.nintendo.znca",
            },
            10, 8 * 1024 * 1024);
        if (page.status == 401 || page.status == 403) clear_cache();

        // Ben Lawrence's zelda-notes-types project confirms the dedicated
        // Zelda Notes service exists, but its repository is not reliably
        // retrievable from this build environment. More importantly, neither
        // nxapi nor our working backend exposes a stable self "current location"
        // endpoint. A successful title-select session is therefore deliberately
        // not turned into invented Discord activity.
        return {};
    } catch (...) {
        return {};
    }
}

}  // namespace nso
