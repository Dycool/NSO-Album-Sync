#include "nso_album_sync/splatnet.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <string>
#include <vector>

namespace nso {
namespace {

constexpr char kBaseUrl[] = "https://api.lp1.av5ja.srv.nintendo.net";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Linux; Android 8.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.125 Mobile Safari/537.36";
constexpr char kSplatNetResourcePrefix[] =
    "https://api.lp1.av5ja.srv.nintendo.net/resources/prod/";
constexpr char kSplatoon3InkAssetPrefix[] =
    "https://splatoon3.ink/assets/splatnet/";
// Current production SplatNet 3 metadata tracked by
// nintendoapis/nintendo-app-versions. Keep this version/hash pair together.
constexpr char kWebViewVersion[] = "10.0.0-4787c271";
constexpr char kHistoryRecordQuery[] =
    "a654ecc80161a7ca5c38761c1d9e502d405eae764e2d343618b9c74b1dc0a80f";
constexpr auto kBulletTtl = std::chrono::minutes(100);

std::string splatnet_language(const std::string& account_language) {
    static constexpr std::array<const char*, 14> kLanguages = {
        "de-DE", "en-GB", "en-US", "es-ES", "es-MX", "fr-CA", "fr-FR",
        "it-IT", "ja-JP", "ko-KR", "nl-NL", "ru-RU", "zh-CN", "zh-TW",
    };
    const auto supported = std::find_if(
        kLanguages.begin(), kLanguages.end(),
        [&](const char* language) { return account_language == language; });
    return supported == kLanguages.end() ? std::string("en-GB") : account_language;
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
        "Accept-Language: en-GB,en-US;q=0.8",
        "X-Requested-With: com.nintendo.znca",
    };
}

std::string discord_weapon_image_url(const std::string& signed_url) {
    const std::string prefix(kSplatNetResourcePrefix);
    if (signed_url.compare(0, prefix.size(), prefix) != 0) {
        // Discord Social SDK permits an external image URL up to 300 chars. If
        // Nintendo ever returns an already-short URL, it is safe to use it as-is.
        return signed_url.size() <= 300 ? signed_url : std::string{};
    }

    auto end = signed_url.find_first_of("?#", prefix.size());
    if (end == std::string::npos) end = signed_url.size();
    if (end <= prefix.size()) return {};

    const auto resource_path = signed_url.substr(prefix.size(), end - prefix.size());
    if (resource_path.find("..") != std::string::npos) return {};
    if (std::any_of(
            resource_path.begin(), resource_path.end(),
            [](unsigned char c) { return std::isspace(c) != 0; })) {
        return {};
    }

    // SplatNet signs its image URLs with very long Expires/Signature/Key-Pair-Id
    // query strings. splatoon3.ink mirrors the same public resources using the
    // normalized path after /resources/prod/, producing a stable URL comfortably
    // below Discord's 300-character asset limit without forwarding the signature.
    const auto mirrored = std::string(kSplatoon3InkAssetPrefix) + resource_path;
    return mirrored.size() <= 300 ? mirrored : std::string{};
}

}  // namespace

void SplatNetClient::clear_cache() {
    std::lock_guard lock(mutex_);
    source_web_token_.clear();
    bullet_token_.clear();
    language_ = splatnet_language(account_language_);
    bullet_expires_at_ = {};
}

std::string SplatNetClient::ensure_bullet_token(const std::string& web_service_token) {
    const auto now = std::chrono::system_clock::now();
    std::string account_language;
    std::string account_country;
    {
        std::lock_guard lock(mutex_);
        if (source_web_token_ == web_service_token && !bullet_token_.empty() &&
            now < bullet_expires_at_) {
            return bullet_token_;
        }
        account_language = account_language_;
        account_country = account_country_;
    }
    const auto api_language = splatnet_language(account_language);

    // nxapi preserves the Nintendo Account language in the launch URL, but the
    // SplatNet API itself only supports a fixed locale list and falls back to
    // en-GB when the account language is unsupported (for example pt-PT).
    const std::string launch = std::string(kBaseUrl) + "/?lang=" + account_language +
        "&na_country=" + account_country + "&na_lang=" + account_language;
    const auto bootstrap = http_.get(
        launch,
        bootstrap_headers(web_service_token),
        10,
        8 * 1024 * 1024);
    if (bootstrap.status != 200) return {};

    const std::vector<std::string> headers = {
        std::string("User-Agent: ") + kUserAgent,
        "Accept: */*",
        std::string("Referrer: ") + kBaseUrl + "/",
        "X-Requested-With: XMLHttpRequest",
        "X-Web-View-Ver: " + std::string(kWebViewVersion),
        "X-NACOUNTRY: " + account_country,
        "Accept-Language: " + api_language,
        "X-GameWebToken: " + web_service_token,
    };
    const auto response = http_.post(
        std::string(kBaseUrl) + "/api/bullet_tokens",
        "",
        headers,
        "application/json",
        10);
    if (response.status != 201) return {};
    const auto json = Json::parse(response.text());
    const auto bullet = json.string("bulletToken");
    if (bullet.empty()) return {};

    std::lock_guard lock(mutex_);
    if (account_language_ != account_language || account_country_ != account_country) {
        return {};
    }
    source_web_token_ = web_service_token;
    bullet_token_ = bullet;
    language_ = json.string("lang", api_language);
    bullet_expires_at_ = now + kBulletTtl;
    return bullet_token_;
}

SplatNetPresence SplatNetClient::fetch_presence(const std::string& web_service_token) {
    if (web_service_token.empty()) return {};
    try {
        const auto bullet = ensure_bullet_token(web_service_token);
        if (bullet.empty()) return {};

        const Json body(Json::object{
            {"variables", Json::object{}},
            {"extensions", Json::object{
                {"persistedQuery", Json::object{
                    {"version", 1},
                    {"sha256Hash", kHistoryRecordQuery},
                }},
            }},
        });
        std::string language;
        {
            std::lock_guard lock(mutex_);
            language = language_;
        }
        const std::vector<std::string> headers = {
            std::string("User-Agent: ") + kUserAgent,
            "Accept: */*",
            std::string("Referrer: ") + kBaseUrl + "/",
            "X-Requested-With: XMLHttpRequest",
            "Authorization: Bearer " + bullet,
            "X-Web-View-Ver: " + std::string(kWebViewVersion),
            "Accept-Language: " + language,
        };
        const auto response = http_.post(
            std::string(kBaseUrl) + "/api/graphql",
            body.dump(),
            headers,
            "application/json",
            10);
        if (response.status == 401 || response.status == 403) clear_cache();
        if (response.status != 200) return {};

        const auto root = Json::parse(response.text());
        const auto* data = root.find("data");
        if (!data || !data->is_object()) return {};
        const auto* player = data->find("currentPlayer");
        if (!player || !player->is_object()) return {};

        SplatNetPresence presence;
        presence.player_name = player->string("name");
        presence.player_id = player->string("nameId");
        presence.title = player->string("byname");
        if (const auto* weapon = player->find("weapon"); weapon && weapon->is_object()) {
            presence.weapon_name = weapon->string("name");
            if (const auto* image = weapon->find("image"); image && image->is_object()) {
                presence.stage_image_uri = discord_weapon_image_url(image->string("url"));
            }
        }
        if (const auto* history = data->find("playHistory"); history && history->is_object()) {
            presence.player_level = history->integer("rank", 0);
            if (const auto* udemae = history->find("udemae"); udemae) {
                if (udemae->is_string()) presence.rank_name = udemae->as_string();
                else if (udemae->is_object()) presence.rank_name = udemae->string("name");
            }
        }
        presence.active = !presence.player_name.empty() ||
            !presence.weapon_name.empty() || !presence.title.empty();
        return presence;
    } catch (...) {
        return {};
    }
}

}  // namespace nso
