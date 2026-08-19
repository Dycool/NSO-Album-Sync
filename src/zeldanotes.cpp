#include "nso_album_sync/zeldanotes.hpp"
#include "nso_album_sync/sse.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr char kBaseUrl[] = "https://api.lp1.87abc152.srv.nintendo.net";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Linux; Android 10; Build/QP1A.190711.020; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/80.0.3987.162 Mobile Safari/537.36 com.nintendo.znca/3.4.1";
constexpr auto kSessionTtl = std::chrono::minutes(90);

std::string header_value(const HttpResponse& response, const std::string& key) {
    const auto it = response.headers.find(key);
    return it == response.headers.end() ? std::string{} : it->second;
}

std::vector<std::string> set_cookie_lines(const HttpResponse& response) {
    const auto cookies = header_value(response, "set-cookie");
    if (cookies.empty()) return {};
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= cookies.size()) {
        const auto end = cookies.find('\n', start);
        auto line = cookies.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string session_cookie(const HttpResponse& response) {
    for (const auto& line : set_cookie_lines(response)) {
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        const auto eq = line.find('=', start);
        if (eq == std::string::npos) continue;
        const auto name = line.substr(start, eq - start);
        auto lower_name = name;
        std::transform(
            lower_name.begin(), lower_name.end(), lower_name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_name != "a5_token" && lower_name.find("session") == std::string::npos) {
            continue;
        }
        const auto value_start = eq + 1;
        auto end = line.find(';', value_start);
        if (end == std::string::npos) end = line.size();
        return name + "=" + line.substr(value_start, end - value_start);
    }
    return {};
}

std::vector<std::string> bootstrap_headers(
    const std::string& token,
    const std::string& language,
    const std::string& country) {
    return {
        "Upgrade-Insecure-Requests: 1",
        std::string("User-Agent: ") + kUserAgent,
        "x-appplatform: android",
        "x-appcolorscheme: DARK",
        "x-gamewebtoken: " + token,
        "dnt: 1",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: " + language,
        "X-NACountry: " + country,
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
        std::string language;
        std::string country;
        {
            std::lock_guard lock(mutex_);
            language = language_;
            country = country_;
            if (source_web_token_ == web_service_token && !session_cookie_.empty() &&
                std::chrono::system_clock::now() < session_expires_at_) {
                // Zelda Notes has no verified structured self-presence endpoint
                // yet. Once the session has been proven, do not keep reloading a
                // page every Discord poll just to rediscover the same cookie.
                return {};
            }
        }

        const auto locale_query = "?lang=" + language + "&na_country=" + country +
            "&na_lang=" + language;

        // The working backend resolves its proxy root to /title-select before
        // making any Nintendo request. Therefore /title-select is the actual
        // first Nintendo navigation that receives GameWebServiceToken and
        // establishes Zelda Notes' a5_token/session cookie.
        const auto bootstrap = http_.get(
            std::string(kBaseUrl) + "/title-select" + locale_query,
            bootstrap_headers(web_service_token, language, country),
            10,
            8 * 1024 * 1024);
        if (bootstrap.status / 100 != 2 && bootstrap.status / 100 != 3) return {};
        const auto cookie = session_cookie(bootstrap);
        if (cookie.empty()) return {};

        std::lock_guard lock(mutex_);
        if (language_ != language || country_ != country) return {};
        source_web_token_ = web_service_token;
        session_cookie_ = cookie;
        session_expires_at_ = std::chrono::system_clock::now() + kSessionTtl;

        // A valid Zelda Notes session is useful proof that probing/auth works,
        // but it is not evidence of Link's current map position or activity.
        return {};
    } catch (...) {
        return {};
    }
}

}  // namespace nso
