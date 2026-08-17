#include "nso_album_sync/util.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace nso {

std::string base64_encode(const std::vector<unsigned char>& data) {
    if (data.empty()) {
        return {};
    }

    std::string encoded(4 * ((data.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        data.data(),
        static_cast<int>(data.size()));

    encoded.resize(length);
    return encoded;
}

std::vector<unsigned char> base64_decode(std::string text) {
    std::replace(text.begin(), text.end(), '-', '+');
    std::replace(text.begin(), text.end(), '_', '/');

    while (text.size() % 4 != 0) {
        text.push_back('=');
    }

    std::vector<unsigned char> decoded((text.size() / 4) * 3 + 3);
    int length = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(text.data()),
        static_cast<int>(text.size()));

    if (length < 0) {
        return {};
    }

    while (!text.empty() && text.back() == '=') {
        --length;
        text.pop_back();
    }

    decoded.resize(static_cast<std::size_t>(std::max(0, length)));
    return decoded;
}

std::string base64url(const std::vector<unsigned char>& data) {
    auto encoded = base64_encode(data);

    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');

    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }

    return encoded;
}

std::vector<unsigned char> random_bytes(std::size_t count) {
    std::vector<unsigned char> bytes(count);

    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    return bytes;
}

std::vector<unsigned char> sha256(const std::string& text) {
    std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH);

    SHA256(
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size(),
        digest.data());

    return digest;
}

std::string url_encode(const std::string& text) {
    std::ostringstream output;
    output << std::hex << std::uppercase;

    for (const unsigned char character : text) {
        const bool unreserved =
            std::isalnum(character) ||
            character == '-' ||
            character == '_' ||
            character == '.' ||
            character == '~';

        if (unreserved) {
            output << static_cast<char>(character);
        } else {
            output << '%'
                   << std::setw(2)
                   << std::setfill('0')
                   << static_cast<int>(character);
        }
    }

    return output.str();
}

std::string lower(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });

    return text;
}

std::string trim(std::string text) {
    const auto is_space = [](const unsigned char character) {
        return std::isspace(character) != 0;
    };

    while (!text.empty() && is_space(text.front())) {
        text.erase(text.begin());
    }

    while (!text.empty() && is_space(text.back())) {
        text.pop_back();
    }

    return text;
}

std::string sanitize_folder(std::string text) {
    if (text.empty()) {
        return "Other";
    }

    for (char& character : text) {
        switch (character) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*':
                character = ' ';
                break;
            default:
                break;
        }
    }

    text = trim(std::move(text));
    return text.empty() ? "Other" : text;
}

std::string normalize_title(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (const unsigned char character : text) {
        if (std::isalnum(character)) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }

    return normalized;
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(
        nullptr,
        "open",
        url.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
#elif __APPLE__
    const std::string command = "open '" + url + "' >/dev/null 2>&1 &";
    std::system(command.c_str());
#else
    const std::string command = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    std::system(command.c_str());
#endif
}

void open_path(const std::filesystem::path& path) {
    open_url(path.string());
}

}  // namespace nso
