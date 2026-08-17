#include "nso_album_sync/http.hpp"
#include "nso_album_sync/util.hpp"

#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nso {
namespace {

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}

    ~InternetHandle() {
        reset();
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    void reset() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HINTERNET handle_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-8 string is too large");
    }

    const int length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        length,
        nullptr,
        0);

    if (required <= 0) {
        throw std::runtime_error("Invalid UTF-8 string");
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            length,
            result.data(),
            required) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }

    return result;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Wide string is too large");
    }

    const int length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        length,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required <= 0) {
        throw std::runtime_error("UTF-8 conversion failed");
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            length,
            result.data(),
            required,
            nullptr,
            nullptr) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }

    return result;
}

[[noreturn]] void throw_winhttp_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed (WinHTTP error " +
        std::to_string(GetLastError()) + ")");
}

ParsedUrl parse_url(const std::string& url) {
    const auto wide_url = utf8_to_wide(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(
            wide_url.c_str(),
            static_cast<DWORD>(wide_url.size()),
            ICU_REJECT_USERPWD,
            &parts)) {
        throw_winhttp_error("WinHttpCrackUrl");
    }

    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Only HTTP(S) URLs are supported");
    }

    ParsedUrl parsed;
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    parsed.port = parts.nPort;
    parsed.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    if (parts.lpszUrlPath != nullptr && parts.dwUrlPathLength != 0) {
        parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    } else {
        parsed.path = L"/";
    }

    if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength != 0) {
        parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    return parsed;
}

std::wstring proxy_server_from_url(const std::string& proxy_url) {
    const auto parsed = parse_url(proxy_url);
    if (parsed.secure) {
        throw std::runtime_error("Only http:// proxies are supported");
    }

    std::wstring proxy = parsed.host;
    if (parsed.port != INTERNET_DEFAULT_HTTP_PORT) {
        proxy += L":" + std::to_wstring(parsed.port);
    }
    return proxy;
}

std::wstring build_headers(
    const std::vector<std::string>& headers,
    const std::string& content_type) {
    std::wstring result = L"Connection: close\r\nAccept-Encoding: identity\r\n";

    for (const auto& header : headers) {
        result += utf8_to_wide(header);
        result += L"\r\n";
    }

    if (!content_type.empty()) {
        result += L"Content-Type: ";
        result += utf8_to_wide(content_type);
        result += L"\r\n";
    }

    return result;
}

std::map<std::string, std::string> read_response_headers(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &bytes,
        WINHTTP_NO_HEADER_INDEX);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return {};
    }

    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            raw.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders");
    }

    raw.resize(wcslen(raw.c_str()));
    std::istringstream lines(wide_to_utf8(raw));
    std::map<std::string, std::string> result;
    std::string line;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        result[lower(line.substr(0, separator))] =
            trim(line.substr(separator + 1));
    }

    return result;
}

std::vector<unsigned char> read_response_body(HINTERNET request) {
    std::vector<unsigned char> body;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw_winhttp_error("WinHttpQueryDataAvailable");
        }

        if (available == 0) {
            break;
        }

        const auto start = body.size();
        body.resize(start + available);

        DWORD read = 0;
        if (!WinHttpReadData(
                request,
                body.data() + start,
                available,
                &read)) {
            throw_winhttp_error("WinHttpReadData");
        }

        body.resize(start + read);
        if (read == 0) {
            break;
        }
    }

    return body;
}

std::string form_component_encode(const std::string& text) {
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

}  // namespace

HttpClient::HttpClient() = default;
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::vector<unsigned char>& body,
    const std::string& content_type,
    long timeout_seconds) const {
    const auto destination = parse_url(url);

    std::wstring proxy;
    DWORD access_type = WINHTTP_ACCESS_TYPE_NO_PROXY;
    LPCWSTR proxy_name = WINHTTP_NO_PROXY_NAME;
    LPCWSTR proxy_bypass = WINHTTP_NO_PROXY_BYPASS;

    if (!proxy_.empty()) {
        proxy = proxy_server_from_url(proxy_);
        access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy_name = proxy.c_str();
    }

    InternetHandle session(WinHttpOpen(
        L"NSO Album Sync/2.0",
        access_type,
        proxy_name,
        proxy_bypass,
        0));
    if (!session) {
        throw_winhttp_error("WinHttpOpen");
    }

    const long safe_timeout = std::max(1L, timeout_seconds);
    const long max_seconds = std::numeric_limits<int>::max() / 1000L;
    const int timeout_ms = static_cast<int>(std::min(safe_timeout, max_seconds) * 1000L);
    if (!WinHttpSetTimeouts(
            session.get(),
            timeout_ms,
            timeout_ms,
            timeout_ms,
            timeout_ms)) {
        throw_winhttp_error("WinHttpSetTimeouts");
    }

    InternetHandle connection(WinHttpConnect(
        session.get(),
        destination.host.c_str(),
        destination.port,
        0));
    if (!connection) {
        throw_winhttp_error("WinHttpConnect");
    }

    const auto wide_method = utf8_to_wide(method);
    const DWORD flags = destination.secure ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        wide_method.c_str(),
        destination.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request) {
        throw_winhttp_error("WinHttpOpenRequest");
    }

    const auto additional_headers = build_headers(headers, content_type);

    if (body.size() > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("HTTP request body is too large");
    }

    LPVOID body_pointer = body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : const_cast<unsigned char*>(body.data());
    const DWORD body_size = static_cast<DWORD>(body.size());

    if (!WinHttpSendRequest(
            request.get(),
            additional_headers.c_str(),
            static_cast<DWORD>(additional_headers.size()),
            body_pointer,
            body_size,
            body_size,
            0)) {
        throw_winhttp_error("WinHttpSendRequest");
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders(status)");
    }

    HttpResponse response;
    response.status = static_cast<long>(status);
    response.headers = read_response_headers(request.get());
    response.body = read_response_body(request.get());
    return response;
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::vector<std::string>& headers,
    long timeout_seconds) const {
    return request("GET", url, headers, {}, "", timeout_seconds);
}

HttpResponse HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers,
    const std::string& content_type,
    long timeout_seconds) const {
    return request(
        "POST",
        url,
        headers,
        std::vector<unsigned char>(body.begin(), body.end()),
        content_type,
        timeout_seconds);
}

HttpResponse HttpClient::post_bytes(
    const std::string& url,
    const std::vector<unsigned char>& body,
    const std::vector<std::string>& headers,
    const std::string& content_type,
    long timeout_seconds) const {
    return request("POST", url, headers, body, content_type, timeout_seconds);
}

std::string HttpClient::form_encode(
    const std::map<std::string, std::string>& values) {
    std::ostringstream result;
    bool first = true;

    for (const auto& [key, value] : values) {
        if (!first) {
            result << '&';
        }
        first = false;
        result << form_component_encode(key) << '=' << form_component_encode(value);
    }

    return result.str();
}

}  // namespace nso

#endif  // _WIN32
