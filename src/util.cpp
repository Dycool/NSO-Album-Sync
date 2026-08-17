#include "nso_album_sync/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <wincrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cerrno>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace nso {
namespace {

#ifdef _WIN32

void ensure_dword_size(std::size_t size, const char* operation) {
    if (size > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error(std::string(operation) + " input is too large");
    }
}

bool nt_success(NTSTATUS status) {
    return status >= 0;
}

#endif

#ifndef _WIN32

bool spawn_detached(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable);
    storage.insert(storage.end(), arguments.begin(), arguments.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    pid_t child = 0;
    const int result = posix_spawnp(
        &child,
        executable.c_str(),
        nullptr,
        nullptr,
        argv.data(),
        environ);

    if (result != 0) {
        return false;
    }

    // Reap the short-lived launcher without blocking the UI thread.
    std::thread([child] {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();

    return true;
}

#endif

}  // namespace

std::string base64_encode(const std::vector<unsigned char>& data) {
    if (data.empty()) {
        return {};
    }

#ifdef _WIN32
    ensure_dword_size(data.size(), "Base64 encode");

    DWORD required = 0;
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &required)) {
        throw std::runtime_error("CryptBinaryToStringA size query failed");
    }

    std::string encoded(required, '\0');
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            encoded.data(),
            &required)) {
        throw std::runtime_error("CryptBinaryToStringA failed");
    }

    if (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
#else
    std::string encoded(4 * ((data.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        data.data(),
        static_cast<int>(data.size()));

    encoded.resize(length);
    return encoded;
#endif
}

std::vector<unsigned char> base64_decode(std::string text) {
    std::replace(text.begin(), text.end(), '-', '+');
    std::replace(text.begin(), text.end(), '_', '/');

    while (text.size() % 4 != 0) {
        text.push_back('=');
    }

#ifdef _WIN32
    ensure_dword_size(text.size(), "Base64 decode");

    DWORD required = 0;
    if (!CryptStringToBinaryA(
            text.c_str(),
            static_cast<DWORD>(text.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &required,
            nullptr,
            nullptr)) {
        return {};
    }

    std::vector<unsigned char> decoded(required);
    if (!CryptStringToBinaryA(
            text.c_str(),
            static_cast<DWORD>(text.size()),
            CRYPT_STRING_BASE64,
            decoded.data(),
            &required,
            nullptr,
            nullptr)) {
        return {};
    }

    decoded.resize(required);
    return decoded;
#else
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
#endif
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
    if (bytes.empty()) {
        return bytes;
    }

#ifdef _WIN32
    ensure_dword_size(count, "Random byte generation");
    if (!nt_success(BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(count),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#else
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
#endif

    return bytes;
}

std::vector<unsigned char> sha256(const std::string& text) {
#ifdef _WIN32
    ensure_dword_size(text.size(), "SHA-256");

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (!nt_success(BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0))) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }

    DWORD hash_length = 0;
    DWORD bytes_returned = 0;
    if (!nt_success(BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_length),
            sizeof(hash_length),
            &bytes_returned,
            0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptGetProperty failed");
    }

    std::vector<unsigned char> digest(hash_length);

    const auto fail = [&](const char* message) -> void {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error(message);
    };

    if (!nt_success(BCryptCreateHash(
            algorithm,
            &hash,
            nullptr,
            0,
            nullptr,
            0,
            0))) {
        fail("BCryptCreateHash failed");
    }

    if (!text.empty() && !nt_success(BCryptHashData(
            hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
            static_cast<ULONG>(text.size()),
            0))) {
        fail("BCryptHashData failed");
    }

    if (!nt_success(BCryptFinishHash(
            hash,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0))) {
        fail("BCryptFinishHash failed");
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
#else
    std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH);

    SHA256(
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size(),
        digest.data());

    return digest;
#endif
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
    if (!spawn_detached("open", {url})) {
        throw std::runtime_error("Failed to launch the default browser");
    }
#else
    if (!spawn_detached("xdg-open", {url})) {
        throw std::runtime_error("Failed to launch the default browser");
    }
#endif
}

void open_path(const std::filesystem::path& path) {
    open_url(path.string());
}

}  // namespace nso
