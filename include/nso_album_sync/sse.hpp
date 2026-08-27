#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "nso_album_sync/windows_compat.hpp"
#include <winhttp.h>
#include <cwchar>
#else
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace nso {

struct ServerSentEvent {
    std::string event;
    std::string data;
    std::string id;
    std::int64_t retry_milliseconds = -1;
};

class ServerSentEventParser {
public:
    using EventCallback = std::function<bool(const ServerSentEvent&)>;

    explicit ServerSentEventParser(std::size_t max_event_bytes = 1024 * 1024)
        : max_event_bytes_(max_event_bytes) {
        if (max_event_bytes_ == 0) {
            throw std::invalid_argument("SSE event size limit must be greater than zero");
        }
    }

    bool feed(const unsigned char* data, std::size_t size, const EventCallback& on_event) {
        if (size != 0 && data == nullptr) {
            throw std::invalid_argument("SSE input buffer is null");
        }
        if (!on_event) {
            throw std::invalid_argument("SSE event callback is empty");
        }

        for (std::size_t index = 0; index < size; ++index) {
            const char character = static_cast<char>(data[index]);
            if (pending_cr_) {
                pending_cr_ = false;
                if (!process_line(on_event)) return false;
                if (character == '\n') continue;
            }

            if (character == '\r') {
                pending_cr_ = true;
            } else if (character == '\n') {
                if (!process_line(on_event)) return false;
            } else {
                line_.push_back(character);
                enforce_limit(line_.size());
            }
        }
        return true;
    }

    bool feed(const std::string& data, const EventCallback& on_event) {
        return feed(
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(), on_event);
    }

    bool finish(const EventCallback& on_event) {
        if (!on_event) {
            throw std::invalid_argument("SSE event callback is empty");
        }
        if (pending_cr_) {
            pending_cr_ = false;
            if (!process_line(on_event)) return false;
        } else if (!line_.empty()) {
            if (!process_line(on_event)) return false;
        }
        line_.clear();
        reset_event_fields();
        return true;
    }

    void reset() {
        line_.clear();
        data_.clear();
        event_.clear();
        last_event_id_.clear();
        retry_milliseconds_ = -1;
        pending_cr_ = false;
    }

private:
    void enforce_limit(std::size_t pending_size) const {
        if (pending_size > max_event_bytes_ ||
            data_.size() > max_event_bytes_ - pending_size) {
            throw std::runtime_error("SSE event exceeded configured size limit");
        }
    }

    static bool valid_retry(const std::string& value) {
        if (value.empty()) return false;
        for (const unsigned char character : value) {
            if (!std::isdigit(character)) return false;
        }
        return true;
    }

    bool process_line(const EventCallback& on_event) {
        std::string line;
        line.swap(line_);
        if (line.empty()) {
            if (data_.empty()) {
                reset_event_fields();
                return true;
            }
            if (data_.back() == '\n') data_.pop_back();
            ServerSentEvent parsed;
            parsed.event = event_.empty() ? "message" : event_;
            parsed.data = std::move(data_);
            parsed.id = last_event_id_;
            parsed.retry_milliseconds = retry_milliseconds_;
            reset_event_fields();
            return on_event(parsed);
        }

        if (line.front() == ':') return true;
        const auto separator = line.find(':');
        const auto field = separator == std::string::npos
            ? line : line.substr(0, separator);
        auto value = separator == std::string::npos
            ? std::string{} : line.substr(separator + 1);
        if (!value.empty() && value.front() == ' ') value.erase(value.begin());

        if (field == "data") {
            enforce_limit(value.size() + 1);
            data_ += value;
            data_.push_back('\n');
        } else if (field == "event") {
            enforce_limit(value.size());
            event_ = std::move(value);
        } else if (field == "id") {
            if (value.find('\0') == std::string::npos) {
                enforce_limit(value.size());
                last_event_id_ = std::move(value);
            }
        } else if (field == "retry" && valid_retry(value)) {
            try {
                retry_milliseconds_ = std::stoll(value);
            } catch (...) {
            }
        }
        return true;
    }

    void reset_event_fields() {
        data_.clear();
        event_.clear();
        retry_milliseconds_ = -1;
    }

    std::size_t max_event_bytes_;
    std::string line_;
    std::string data_;
    std::string event_;
    std::string last_event_id_;
    std::int64_t retry_milliseconds_ = -1;
    bool pending_cr_ = false;
};

struct SseResponse {
    long status = 0;
    std::map<std::string, std::string> headers;
};

class SseClient {
public:
    using EventCallback = ServerSentEventParser::EventCallback;
    using CancelCallback = std::function<bool()>;

    SseClient() = default;
    explicit SseClient(std::string proxy_url) : proxy_url_(std::move(proxy_url)) {}

    void set_proxy(std::string proxy_url) {
        std::lock_guard lock(proxy_mutex_);
        proxy_url_ = std::move(proxy_url);
    }

    SseResponse stream(
        const std::string& url,
        const std::vector<std::string>& headers,
        const EventCallback& on_event,
        const CancelCallback& should_cancel = {},
        long connect_timeout_seconds = 30,
        std::size_t max_event_bytes = 1024 * 1024) const;

private:
    std::string proxy() const {
        std::lock_guard lock(proxy_mutex_);
        return proxy_url_;
    }

    mutable std::mutex proxy_mutex_;
    std::string proxy_url_;
};

namespace sse_detail {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kReadBufferBytes = 16 * 1024;
constexpr std::size_t kMaxChunkBytes = 8 * 1024 * 1024;
constexpr long kReadPollSeconds = 1;

inline bool cancelled(const SseClient::CancelCallback& should_cancel) {
    return should_cancel && should_cancel();
}

inline std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return text;
}

inline std::string trim_left(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    return text;
}

inline long safe_timeout(long timeout_seconds) {
    return std::clamp(timeout_seconds, 1L, 24L * 60L * 60L);
}

inline SseResponse parse_response_head(const std::string& header_text) {
    SseResponse response;
    std::istringstream lines(header_text);
    std::string line;
    if (!std::getline(lines, line)) {
        throw std::runtime_error("Invalid SSE HTTP response");
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream status_line(line);
    std::string protocol;
    status_line >> protocol >> response.status;
    if (protocol.rfind("HTTP/", 0) != 0 || response.status <= 0) {
        throw std::runtime_error("Invalid SSE HTTP status line");
    }
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        const auto name = lowercase(line.substr(0, separator));
        const auto value = trim_left(line.substr(separator + 1));
        const auto existing = response.headers.find(name);
        if (name == "set-cookie" && existing != response.headers.end()) {
            existing->second += "\n" + value;
        } else {
            response.headers[name] = value;
        }
    }
    return response;
}

class ChunkedDecoder {
public:
    using DataCallback = std::function<bool(const unsigned char*, std::size_t)>;

    bool feed(const unsigned char* data, std::size_t size, const DataCallback& on_data) {
        if (done_) return true;
        buffer_.append(reinterpret_cast<const char*>(data), size);
        for (;;) {
            if (reading_trailers_) {
                const auto line_end = buffer_.find("\r\n");
                if (line_end == std::string::npos) {
                    if (buffer_.size() > kMaxHeaderBytes) {
                        throw std::runtime_error("Chunked SSE trailers too large");
                    }
                    return true;
                }
                if (line_end == 0) {
                    buffer_.erase(0, 2);
                    done_ = true;
                    return true;
                }
                buffer_.erase(0, line_end + 2);
                continue;
            }

            if (remaining_ == 0) {
                const auto line_end = buffer_.find("\r\n");
                if (line_end == std::string::npos) {
                    if (buffer_.size() > 128) {
                        throw std::runtime_error("Invalid chunked SSE response");
                    }
                    return true;
                }
                auto length_text = buffer_.substr(0, line_end);
                if (const auto extension = length_text.find(';');
                    extension != std::string::npos) {
                    length_text.resize(extension);
                }
                std::size_t consumed = 0;
                std::size_t length = 0;
                try {
                    length = std::stoull(length_text, &consumed, 16);
                } catch (...) {
                    throw std::runtime_error("Invalid chunked SSE response");
                }
                if (consumed != length_text.size() || length > kMaxChunkBytes) {
                    throw std::runtime_error("Invalid chunked SSE response");
                }
                buffer_.erase(0, line_end + 2);
                if (length == 0) {
                    reading_trailers_ = true;
                    continue;
                }
                remaining_ = length;
            }

            if (buffer_.size() < remaining_ + 2) return true;
            if (!on_data(
                    reinterpret_cast<const unsigned char*>(buffer_.data()),
                    remaining_)) {
                return false;
            }
            if (buffer_[remaining_] != '\r' || buffer_[remaining_ + 1] != '\n') {
                throw std::runtime_error("Invalid chunked SSE response");
            }
            buffer_.erase(0, remaining_ + 2);
            remaining_ = 0;
        }
    }

    bool done() const { return done_; }

private:
    std::string buffer_;
    std::size_t remaining_ = 0;
    bool reading_trailers_ = false;
    bool done_ = false;
};

#ifdef _WIN32

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

inline std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("SSE string is too large");
    }
    const int length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (required <= 0) throw std::runtime_error("Invalid UTF-8 string");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
            result.data(), required) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

inline std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("SSE string is too large");
    }
    const int length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

[[noreturn]] inline void throw_winhttp_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed (WinHTTP error " +
        std::to_string(GetLastError()) + ")");
}

inline ParsedUrl parse_url(const std::string& url) {
    const auto wide_url = utf8_to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            wide_url.c_str(), static_cast<DWORD>(wide_url.size()),
            ICU_REJECT_USERPWD, &parts)) {
        throw_winhttp_error("WinHttpCrackUrl");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Only HTTP(S) SSE URLs are supported");
    }
    ParsedUrl parsed;
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    parsed.port = parts.nPort;
    parsed.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    parsed.path = parts.dwUrlPathLength == 0
        ? L"/" : std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength != 0) {
        parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    return parsed;
}

inline std::wstring proxy_server_from_url(const std::string& proxy_url) {
    const auto parsed = parse_url(proxy_url);
    if (parsed.secure) throw std::runtime_error("Only http:// proxies are supported");
    std::wstring proxy = parsed.host;
    if (parsed.port != INTERNET_DEFAULT_HTTP_PORT) {
        proxy += L":" + std::to_wstring(parsed.port);
    }
    return proxy;
}

inline std::wstring build_headers(const std::vector<std::string>& headers) {
    std::wstring result =
        L"Connection: keep-alive\r\nAccept-Encoding: identity\r\n";
    for (const auto& header : headers) {
        result += utf8_to_wide(header);
        result += L"\r\n";
    }
    return result;
}

inline std::map<std::string, std::string> read_headers(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
            raw.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders");
    }
    raw.resize(wcslen(raw.c_str()));
    std::istringstream lines(wide_to_utf8(raw));
    std::map<std::string, std::string> result;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        const auto name = lowercase(line.substr(0, separator));
        const auto value = trim_left(line.substr(separator + 1));
        const auto existing = result.find(name);
        if (name == "set-cookie" && existing != result.end()) {
            existing->second += "\n" + value;
        } else {
            result[name] = value;
        }
    }
    return result;
}

inline SseResponse stream_windows(
    const std::string& url,
    const std::string& proxy_url,
    const std::vector<std::string>& headers,
    const SseClient::EventCallback& on_event,
    const SseClient::CancelCallback& should_cancel,
    long connect_timeout_seconds,
    std::size_t max_event_bytes) {
    if (cancelled(should_cancel)) return {};
    const auto destination = parse_url(url);

    std::wstring proxy;
    DWORD access_type = WINHTTP_ACCESS_TYPE_NO_PROXY;
    LPCWSTR proxy_name = WINHTTP_NO_PROXY_NAME;
    if (!proxy_url.empty()) {
        proxy = proxy_server_from_url(proxy_url);
        access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy_name = proxy.c_str();
    }

    InternetHandle session(WinHttpOpen(
        L"NSO Album Sync/2.0", access_type, proxy_name,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw_winhttp_error("WinHttpOpen");

    const long timeout = safe_timeout(connect_timeout_seconds);
    const long max_seconds = (std::numeric_limits<int>::max)() / 1000L;
    const int timeout_ms = static_cast<int>(std::min(timeout, max_seconds) * 1000L);
    if (!WinHttpSetTimeouts(
            session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms)) {
        throw_winhttp_error("WinHttpSetTimeouts");
    }

    InternetHandle connection(WinHttpConnect(
        session.get(), destination.host.c_str(), destination.port, 0));
    if (!connection) throw_winhttp_error("WinHttpConnect");

    const DWORD flags = destination.secure ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", destination.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) throw_winhttp_error("WinHttpOpenRequest");

    const auto additional_headers = build_headers(headers);
    if (!WinHttpSendRequest(
            request.get(), additional_headers.c_str(),
            static_cast<DWORD>(additional_headers.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        throw_winhttp_error("WinHttpSendRequest");
    }
    if (cancelled(should_cancel)) return {};
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders(status)");
    }

    SseResponse response;
    response.status = static_cast<long>(status);
    response.headers = read_headers(request.get());

    DWORD receive_timeout = static_cast<DWORD>(kReadPollSeconds * 1000L);
    if (!WinHttpSetOption(
            request.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT,
            &receive_timeout, sizeof(receive_timeout))) {
        throw_winhttp_error("WinHttpSetOption(receive timeout)");
    }

    ServerSentEventParser parser(max_event_bytes);
    std::array<unsigned char, kReadBufferBytes> buffer{};
    for (;;) {
        if (cancelled(should_cancel)) return response;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) continue;
            throw_winhttp_error("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            parser.finish(on_event);
            return response;
        }

        while (available > 0) {
            if (cancelled(should_cancel)) return response;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                available, buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), requested, &read)) {
                if (GetLastError() == ERROR_WINHTTP_TIMEOUT) break;
                throw_winhttp_error("WinHttpReadData");
            }
            if (read == 0) {
                parser.finish(on_event);
                return response;
            }
            if (!parser.feed(buffer.data(), read, on_event)) return response;
            available -= std::min(available, read);
        }
    }
}

#else

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string path;
    int port = 443;
};

struct Connection {
    BIO* raw = nullptr;
    BIO* io = nullptr;
    SSL_CTX* context = nullptr;
    SSL* ssl = nullptr;

    ~Connection() {
        if (io != nullptr && io != raw) BIO_free(io);
        if (ssl != nullptr) SSL_free(ssl);
        if (context != nullptr) SSL_CTX_free(context);
        if (raw != nullptr) BIO_free_all(raw);
    }

    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};

inline ParsedUrl parse_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) throw std::runtime_error("Invalid SSE URL");
    ParsedUrl parsed;
    parsed.scheme = lowercase(url.substr(0, scheme_end));
    const auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    const auto query_start = url.find('?', authority_start);
    if (path_start == std::string::npos ||
        (query_start != std::string::npos && query_start < path_start)) {
        path_start = query_start;
    }
    const auto authority = url.substr(
        authority_start,
        path_start == std::string::npos
            ? url.size() - authority_start
            : path_start - authority_start);
    parsed.path = path_start == std::string::npos ? "/" : url.substr(path_start);
    if (!parsed.path.empty() && parsed.path.front() == '?') {
        parsed.path.insert(parsed.path.begin(), '/');
    }

    const auto port_separator = authority.rfind(':');
    const bool looks_like_ipv6 = authority.find(']') != std::string::npos;
    if (port_separator != std::string::npos && !looks_like_ipv6) {
        parsed.host = authority.substr(0, port_separator);
        parsed.port = std::stoi(authority.substr(port_separator + 1));
    } else {
        parsed.host = authority;
        parsed.port = parsed.scheme == "http" ? 80 : 443;
    }
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        throw std::runtime_error("Only HTTP(S) SSE URLs are supported");
    }
    return parsed;
}

inline void apply_socket_timeout(BIO* bio, long timeout_seconds) {
    int fd = -1;
    if (bio == nullptr || BIO_get_fd(bio, &fd) < 0 || fd < 0) return;
    const timeval timeout{static_cast<time_t>(safe_timeout(timeout_seconds)), 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("Could not configure SSE socket timeout");
    }
}

inline void wait_for_connect(
    BIO* bio,
    std::chrono::steady_clock::time_point deadline,
    const SseClient::CancelCallback& should_cancel) {
    int fd = -1;
    if (BIO_get_fd(bio, &fd) < 0 || fd < 0) {
        throw std::runtime_error("SSE TCP connection setup failed");
    }
    for (;;) {
        if (cancelled(should_cancel)) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("SSE TCP connection timed out");
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - now);
        const auto slice = std::min<std::int64_t>(remaining.count(), 250'000);
        timeval timeout{
            static_cast<time_t>(slice / 1'000'000),
            static_cast<suseconds_t>(slice % 1'000'000)};
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(fd, &read_set);
        FD_SET(fd, &write_set);
        const int ready = select(fd + 1, &read_set, &write_set, nullptr, &timeout);
        if (ready > 0) return;
        if (ready == 0) continue;
        if (errno != EINTR) throw std::runtime_error("SSE TCP connection wait failed");
    }
}

inline bool connect_tcp(
    Connection& connection,
    const std::string& endpoint,
    long timeout_seconds,
    const SseClient::CancelCallback& should_cancel) {
    connection.raw = BIO_new_connect(endpoint.c_str());
    if (connection.raw == nullptr) {
        throw std::runtime_error("SSE TCP connection setup failed");
    }
    BIO_set_nbio(connection.raw, 1);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(safe_timeout(timeout_seconds));
    for (;;) {
        if (cancelled(should_cancel)) return false;
        if (BIO_do_connect(connection.raw) > 0) break;
        if (!BIO_should_retry(connection.raw)) {
            throw std::runtime_error("SSE TCP connection failed");
        }
        wait_for_connect(connection.raw, deadline, should_cancel);
    }
    BIO_set_nbio(connection.raw, 0);
    int fd = -1;
    if (BIO_get_fd(connection.raw, &fd) >= 0 && fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
    }
    apply_socket_timeout(connection.raw, timeout_seconds);
    connection.io = connection.raw;
    return true;
}

inline std::string read_until(
    BIO* bio,
    const std::string& marker,
    long timeout_seconds,
    const SseClient::CancelCallback& should_cancel) {
    std::string result;
    char buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(safe_timeout(timeout_seconds));
    while (result.find(marker) == std::string::npos) {
        if (cancelled(should_cancel)) return {};
        const int bytes_read = BIO_read(bio, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            if (BIO_should_retry(bio) && std::chrono::steady_clock::now() < deadline) {
                continue;
            }
            if (BIO_should_retry(bio)) {
                throw std::runtime_error("SSE HTTP read timed out");
            }
            throw std::runtime_error("Connection closed while reading SSE HTTP headers");
        }
        result.append(buffer, bytes_read);
        if (result.size() > kMaxHeaderBytes) {
            throw std::runtime_error("SSE HTTP headers too large");
        }
    }
    return result;
}

inline bool establish_proxy_tunnel(
    Connection& connection,
    const ParsedUrl& destination,
    long timeout_seconds,
    const SseClient::CancelCallback& should_cancel) {
    const auto authority = destination.host + ":" + std::to_string(destination.port);
    const std::string request =
        "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority +
        "\r\nProxy-Connection: Keep-Alive\r\n\r\n";
    if (BIO_write(connection.raw, request.data(), static_cast<int>(request.size())) !=
        static_cast<int>(request.size())) {
        throw std::runtime_error("SSE proxy CONNECT write failed");
    }
    const auto response_headers = read_until(
        connection.raw, "\r\n\r\n", timeout_seconds, should_cancel);
    if (cancelled(should_cancel)) return false;
    const bool accepted =
        response_headers.rfind("HTTP/1.1 200", 0) == 0 ||
        response_headers.rfind("HTTP/1.0 200", 0) == 0 ||
        response_headers.find(" 200 ") != std::string::npos;
    if (!accepted) throw std::runtime_error("SSE HTTP proxy CONNECT failed");
    return true;
}

inline void wait_for_ssl(SSL* ssl, int error, std::chrono::steady_clock::time_point deadline) {
    int fd = SSL_get_fd(ssl);
    if (fd < 0) throw std::runtime_error("TLS socket descriptor error");

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("TLS handshake timed out");
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    timeval timeout{
        static_cast<time_t>(remaining.count() / 1'000'000),
        static_cast<suseconds_t>(remaining.count() % 1'000'000),
    };

    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (error == SSL_ERROR_WANT_READ) FD_SET(fd, &read_set);
    if (error == SSL_ERROR_WANT_WRITE) FD_SET(fd, &write_set);
    const int ready = select(fd + 1, &read_set, &write_set, nullptr, &timeout);
    if (ready == 0) throw std::runtime_error("TLS handshake timed out");
    if (ready < 0 && errno != EINTR) throw std::runtime_error("TLS handshake wait failed");
}

inline void enable_tls(Connection& connection, const ParsedUrl& destination) {
    connection.context = SSL_CTX_new(TLS_client_method());
    if (connection.context == nullptr) throw std::runtime_error("SSL_CTX_new failed");
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(connection.context, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    SSL_CTX_set_verify(connection.context, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(connection.context) != 1) {
        throw std::runtime_error("Could not load system TLS trust store");
    }
    connection.ssl = SSL_new(connection.context);
    if (connection.ssl == nullptr) throw std::runtime_error("SSL_new failed");
    SSL_set_tlsext_host_name(connection.ssl, destination.host.c_str());
    if (SSL_set1_host(connection.ssl, destination.host.c_str()) != 1) {
        throw std::runtime_error("Could not configure TLS hostname verification");
    }
    SSL_set_bio(connection.ssl, connection.raw, connection.raw);
    connection.raw = nullptr;
    connection.io = nullptr;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const int result = SSL_connect(connection.ssl);
        if (result == 1) break;
        const int error = SSL_get_error(connection.ssl, result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            wait_for_ssl(connection.ssl, error, deadline);
            continue;
        }
        if (error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_for_ssl(connection.ssl, SSL_ERROR_WANT_READ, deadline);
            continue;
        }
        char err_buf[256];
        ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
        throw std::runtime_error(std::string("SSE TLS handshake failed: ") + err_buf);
    }

    connection.io = BIO_new(BIO_f_ssl());
    if (connection.io == nullptr) {
        throw std::runtime_error("BIO_new(BIO_f_ssl()) failed");
    }
    BIO_set_ssl(connection.io, connection.ssl, BIO_NOCLOSE);
}

inline void write_all(Connection& connection, bool use_tls, const std::string& data) {
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int requested = static_cast<int>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int written = use_tls
            ? SSL_write(connection.ssl, cursor, requested)
            : BIO_write(connection.io, cursor, requested);
        if (written <= 0) throw std::runtime_error("SSE HTTP write failed");
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

inline int read_some(
    Connection& connection,
    bool use_tls,
    unsigned char* buffer,
    std::size_t capacity,
    const SseClient::CancelCallback& should_cancel) {
    for (;;) {
        if (cancelled(should_cancel)) return -2;
        const int requested = static_cast<int>(std::min<std::size_t>(
            capacity, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int bytes_read = use_tls
            ? SSL_read(connection.ssl, buffer, requested)
            : BIO_read(connection.io, buffer, requested);
        if (bytes_read > 0) return bytes_read;
        if (use_tls) {
            const int error = SSL_get_error(connection.ssl, bytes_read);
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            if (error == SSL_ERROR_SYSCALL) {
                if (errno == 0) return 0;
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
            }
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            throw std::runtime_error("SSE HTTP read failed");
        }
        if (BIO_should_retry(connection.io)) continue;
        return 0;
    }
}

inline std::string build_request(
    const std::string& url,
    const ParsedUrl& destination,
    bool use_proxy,
    const std::vector<std::string>& headers) {
    const bool plain_http_proxy = use_proxy && destination.scheme == "http";
    const auto request_target = plain_http_proxy ? url : destination.path;
    std::ostringstream request;
    request << "GET " << request_target << " HTTP/1.1\r\nHost: " << destination.host;
    const bool non_default_port =
        (destination.scheme == "https" && destination.port != 443) ||
        (destination.scheme == "http" && destination.port != 80);
    if (non_default_port) request << ':' << destination.port;
    request << "\r\nConnection: keep-alive\r\nAccept-Encoding: identity\r\n";
    for (const auto& header : headers) request << header << "\r\n";
    request << "\r\n";
    return request.str();
}

inline SseResponse stream_openssl(
    const std::string& url,
    const std::string& proxy_url,
    const std::vector<std::string>& headers,
    const SseClient::EventCallback& on_event,
    const SseClient::CancelCallback& should_cancel,
    long connect_timeout_seconds,
    std::size_t max_event_bytes) {
    if (cancelled(should_cancel)) return {};
    OPENSSL_init_ssl(0, nullptr);
    const auto destination = parse_url(url);
    const bool use_proxy = !proxy_url.empty();
    ParsedUrl proxy;
    if (use_proxy) {
        proxy = parse_url(proxy_url);
        if (proxy.scheme != "http") {
            throw std::runtime_error("Only http:// proxies are supported");
        }
    }
    const auto endpoint_host = use_proxy ? proxy.host : destination.host;
    const auto endpoint_port = use_proxy ? proxy.port : destination.port;

    Connection connection;
    if (!connect_tcp(
            connection,
            endpoint_host + ":" + std::to_string(endpoint_port),
            connect_timeout_seconds, should_cancel)) {
        return {};
    }
    if (use_proxy && destination.scheme == "https" &&
        !establish_proxy_tunnel(
            connection, destination, connect_timeout_seconds, should_cancel)) {
        return {};
    }
    const bool use_tls = destination.scheme == "https";
    if (use_tls) enable_tls(connection, destination);
    if (cancelled(should_cancel)) return {};

    write_all(connection, use_tls, build_request(url, destination, use_proxy, headers));
    apply_socket_timeout(
        use_tls ? SSL_get_rbio(connection.ssl) : connection.io,
        kReadPollSeconds);

    std::array<unsigned char, kReadBufferBytes> buffer{};
    std::string head_bytes;
    std::string body_prefix;
    for (;;) {
        const int bytes_read = read_some(
            connection, use_tls, buffer.data(), buffer.size(), should_cancel);
        if (bytes_read == -2) return {};
        if (bytes_read == 0) {
            throw std::runtime_error("Connection closed before SSE HTTP headers");
        }
        head_bytes.append(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::size_t>(bytes_read));
        const auto separator = head_bytes.find("\r\n\r\n");
        if (separator != std::string::npos) {
            body_prefix = head_bytes.substr(separator + 4);
            head_bytes.resize(separator);
            break;
        }
        if (head_bytes.size() > kMaxHeaderBytes) {
            throw std::runtime_error("SSE HTTP headers too large");
        }
    }

    auto response = parse_response_head(head_bytes);
    if (const auto encoding = response.headers.find("content-encoding");
        encoding != response.headers.end() &&
        lowercase(encoding->second) != "identity") {
        throw std::runtime_error("Compressed SSE responses are not supported");
    }

    ServerSentEventParser parser(max_event_bytes);
    const auto deliver = [&](const unsigned char* data, std::size_t size) {
        return parser.feed(data, size, on_event);
    };

    const auto transfer = response.headers.find("transfer-encoding");
    const bool chunked = transfer != response.headers.end() &&
        lowercase(transfer->second).find("chunked") != std::string::npos;
    std::size_t remaining = (std::numeric_limits<std::size_t>::max)();
    bool has_length = false;
    if (const auto length = response.headers.find("content-length");
        length != response.headers.end()) {
        try {
            remaining = static_cast<std::size_t>(std::stoull(length->second));
            has_length = true;
        } catch (...) {
            throw std::runtime_error("Invalid SSE Content-Length header");
        }
    }

    ChunkedDecoder decoder;
    auto consume = [&](const unsigned char* data, std::size_t size) -> bool {
        if (chunked) return decoder.feed(data, size, deliver);
        if (has_length) {
            const auto count = std::min(remaining, size);
            if (count != 0 && !deliver(data, count)) return false;
            remaining -= count;
            return true;
        }
        return deliver(data, size);
    };

    if (!body_prefix.empty() &&
        !consume(
            reinterpret_cast<const unsigned char*>(body_prefix.data()),
            body_prefix.size())) {
        return response;
    }

    for (;;) {
        if (cancelled(should_cancel)) return response;
        if ((chunked && decoder.done()) || (has_length && remaining == 0)) {
            parser.finish(on_event);
            return response;
        }
        const int bytes_read = read_some(
            connection, use_tls, buffer.data(), buffer.size(), should_cancel);
        if (bytes_read == -2) return response;
        if (bytes_read == 0) {
            parser.finish(on_event);
            return response;
        }
        if (!consume(buffer.data(), static_cast<std::size_t>(bytes_read))) {
            return response;
        }
    }
}

#endif

}  // namespace sse_detail

inline SseResponse SseClient::stream(
    const std::string& url,
    const std::vector<std::string>& headers,
    const EventCallback& on_event,
    const CancelCallback& should_cancel,
    long connect_timeout_seconds,
    std::size_t max_event_bytes) const {
    if (!on_event) throw std::invalid_argument("SSE event callback is empty");
#ifdef _WIN32
    return sse_detail::stream_windows(
        url, proxy(), headers, on_event, should_cancel,
        connect_timeout_seconds, max_event_bytes);
#else
    return sse_detail::stream_openssl(
        url, proxy(), headers, on_event, should_cancel,
        connect_timeout_seconds, max_event_bytes);
#endif
}

}  // namespace nso
