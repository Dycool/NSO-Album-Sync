#include "nso_album_sync/http.hpp"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace nso {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kReadBufferBytes = 16 * 1024;

std::once_flag g_ssl_once;

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

ParsedUrl parse_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::runtime_error("Invalid URL");
    }

    ParsedUrl parsed;
    parsed.scheme = url.substr(0, scheme_end);
    const auto authority_start = scheme_end + 3;
    const auto path_start = url.find('/', authority_start);
    const auto authority = url.substr(
        authority_start,
        path_start == std::string::npos
            ? url.size() - authority_start
            : path_start - authority_start);
    parsed.path = path_start == std::string::npos ? "/" : url.substr(path_start);

    const auto port_separator = authority.rfind(':');
    const bool looks_like_ipv6 = authority.find(']') != std::string::npos;
    if (port_separator != std::string::npos && !looks_like_ipv6) {
        parsed.host = authority.substr(0, port_separator);
        parsed.port = std::stoi(authority.substr(port_separator + 1));
    } else {
        parsed.host = authority;
        parsed.port = parsed.scheme == "http" ? 80 : 443;
    }
    return parsed;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return text;
}

std::string trim_left(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    return text;
}

long safe_timeout(long timeout_seconds) {
    return std::clamp(timeout_seconds, 1L, 24L * 60L * 60L);
}

void apply_socket_timeout(BIO* bio, long timeout_seconds) {
    int fd = -1;
    if (bio == nullptr || BIO_get_fd(bio, &fd) < 0 || fd < 0) return;

    const timeval timeout{
        static_cast<time_t>(safe_timeout(timeout_seconds)),
        0,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("Could not configure HTTP socket timeout");
    }
}

void wait_for_connect(BIO* bio, std::chrono::steady_clock::time_point deadline) {
    int fd = -1;
    if (BIO_get_fd(bio, &fd) < 0 || fd < 0) {
        throw std::runtime_error("TCP connection setup failed");
    }

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("TCP connection timed out");
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - now);
        timeval timeout{
            static_cast<time_t>(remaining.count() / 1'000'000),
            static_cast<suseconds_t>(remaining.count() % 1'000'000),
        };

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(fd, &read_set);
        FD_SET(fd, &write_set);
        const int ready = select(fd + 1, &read_set, &write_set, nullptr, &timeout);
        if (ready > 0) return;
        if (ready == 0) throw std::runtime_error("TCP connection timed out");
        if (errno != EINTR) throw std::runtime_error("TCP connection wait failed");
    }
}

void connect_tcp(
    Connection& connection,
    const std::string& endpoint,
    long timeout_seconds) {
    connection.raw = BIO_new_connect(endpoint.c_str());
    if (connection.raw == nullptr) {
        throw std::runtime_error("TCP connection setup failed");
    }

    BIO_set_nbio(connection.raw, 1);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(safe_timeout(timeout_seconds));
    for (;;) {
        if (BIO_do_connect(connection.raw) > 0) break;
        if (!BIO_should_retry(connection.raw)) {
            throw std::runtime_error("TCP connection failed");
        }
        wait_for_connect(connection.raw, deadline);
    }
    BIO_set_nbio(connection.raw, 0);
    apply_socket_timeout(connection.raw, timeout_seconds);
    connection.io = connection.raw;
}

std::string read_until(BIO* bio, const std::string& marker) {
    std::string result;
    char buffer[1024];
    while (result.find(marker) == std::string::npos) {
        const int bytes_read = BIO_read(bio, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            if (BIO_should_retry(bio)) {
                throw std::runtime_error("HTTP read timed out");
            }
            throw std::runtime_error("Connection closed while reading HTTP headers");
        }
        result.append(buffer, bytes_read);
        if (result.size() > kMaxHeaderBytes) {
            throw std::runtime_error("HTTP headers too large");
        }
    }
    return result;
}

void establish_proxy_tunnel(
    Connection& connection,
    const ParsedUrl& destination) {
    const auto authority = destination.host + ":" + std::to_string(destination.port);
    const std::string request =
        "CONNECT " + authority + " HTTP/1.1\r\n" +
        "Host: " + authority + "\r\n" +
        "Proxy-Connection: Keep-Alive\r\n\r\n";
    if (BIO_write(connection.raw, request.data(), static_cast<int>(request.size())) !=
        static_cast<int>(request.size())) {
        throw std::runtime_error("Proxy CONNECT write failed");
    }
    const auto response_headers = read_until(connection.raw, "\r\n\r\n");
    const bool proxy_accepted =
        response_headers.rfind("HTTP/1.1 200", 0) == 0 ||
        response_headers.rfind("HTTP/1.0 200", 0) == 0 ||
        response_headers.find(" 200 ") != std::string::npos;
    if (!proxy_accepted) throw std::runtime_error("HTTP proxy CONNECT failed");
}

void enable_tls(Connection& connection, const ParsedUrl& destination) {
    connection.context = SSL_CTX_new(TLS_client_method());
    if (connection.context == nullptr) throw std::runtime_error("SSL_CTX_new failed");
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
    const int connect_result = SSL_connect(connection.ssl);
    if (connect_result != 1) {
        const int error = SSL_get_error(connection.ssl, connect_result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
            (error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            throw std::runtime_error("TLS handshake timed out");
        }
        throw std::runtime_error("TLS handshake failed");
    }

    connection.io = BIO_new(BIO_f_ssl());
    if (connection.io == nullptr) throw std::runtime_error("BIO_new(BIO_f_ssl()) failed");
    BIO_set_ssl(connection.io, connection.ssl, BIO_NOCLOSE);
}

void write_all(
    Connection& connection,
    bool use_tls,
    const unsigned char* data,
    std::size_t length) {
    while (length > 0) {
        const auto request_length = static_cast<int>(std::min<std::size_t>(
            length, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int written = use_tls
            ? SSL_write(connection.ssl, data, request_length)
            : BIO_write(connection.io, data, request_length);
        if (written <= 0) {
            if (use_tls) {
                const int error = SSL_get_error(connection.ssl, written);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                    (error == SSL_ERROR_SYSCALL &&
                     (errno == EAGAIN || errno == EWOULDBLOCK))) {
                    throw std::runtime_error("HTTP write timed out");
                }
            } else if (BIO_should_retry(connection.io)) {
                throw std::runtime_error("HTTP write timed out");
            }
            throw std::runtime_error("HTTP write failed");
        }
        data += written;
        length -= static_cast<std::size_t>(written);
    }
}

std::vector<unsigned char> read_all(
    Connection& connection,
    bool use_tls,
    std::size_t max_response_bytes) {
    std::vector<unsigned char> response;
    unsigned char buffer[kReadBufferBytes];

    std::size_t total_limit = 0;
    if (max_response_bytes != 0) {
        if (max_response_bytes >
            (std::numeric_limits<std::size_t>::max)() - kMaxHeaderBytes) {
            total_limit = (std::numeric_limits<std::size_t>::max)();
        } else {
            total_limit = max_response_bytes + kMaxHeaderBytes;
        }
    }

    for (;;) {
        const int bytes_read = use_tls
            ? SSL_read(connection.ssl, buffer, sizeof(buffer))
            : BIO_read(connection.io, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            const auto count = static_cast<std::size_t>(bytes_read);
            if (total_limit != 0 &&
                (response.size() > total_limit || count > total_limit - response.size())) {
                throw std::runtime_error("HTTP response exceeded configured size limit");
            }
            response.insert(response.end(), buffer, buffer + bytes_read);
            continue;
        }

        if (use_tls) {
            const int error = SSL_get_error(connection.ssl, bytes_read);
            if (error == SSL_ERROR_ZERO_RETURN) break;
            if (error == SSL_ERROR_SYSCALL) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    throw std::runtime_error("HTTP read timed out");
                }
                if (errno == 0) break;
            }
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                throw std::runtime_error("HTTP read timed out");
            }
            throw std::runtime_error("HTTP read failed");
        }

        if (BIO_should_retry(connection.io)) {
            throw std::runtime_error("HTTP read timed out");
        }
        break;
    }
    return response;
}

std::vector<unsigned char> decode_chunked(const std::vector<unsigned char>& encoded) {
    std::vector<unsigned char> decoded;
    std::size_t position = 0;
    for (;;) {
        auto line_end = position;
        while (line_end + 1 < encoded.size() &&
               !(encoded[line_end] == '\r' && encoded[line_end + 1] == '\n')) {
            ++line_end;
        }
        if (line_end + 1 >= encoded.size()) {
            throw std::runtime_error("Incomplete chunked HTTP response");
        }

        std::string length_text(
            reinterpret_cast<const char*>(encoded.data()) + position,
            line_end - position);
        if (const auto extension = length_text.find(';'); extension != std::string::npos) {
            length_text.resize(extension);
        }
        std::size_t consumed = 0;
        std::size_t chunk_length = 0;
        try {
            chunk_length = std::stoull(length_text, &consumed, 16);
        } catch (...) {
            throw std::runtime_error("Invalid chunked HTTP response");
        }
        if (consumed != length_text.size()) {
            throw std::runtime_error("Invalid chunked HTTP response");
        }

        position = line_end + 2;
        if (chunk_length == 0) return decoded;
        if (position + chunk_length + 2 > encoded.size()) {
            throw std::runtime_error("Incomplete chunked HTTP response");
        }
        decoded.insert(
            decoded.end(),
            encoded.begin() + static_cast<std::ptrdiff_t>(position),
            encoded.begin() + static_cast<std::ptrdiff_t>(position + chunk_length));
        position += chunk_length;
        if (encoded[position] != '\r' || encoded[position + 1] != '\n') {
            throw std::runtime_error("Invalid chunked HTTP response");
        }
        position += 2;
    }
}

std::string build_request_headers(
    const std::string& method,
    const std::string& original_url,
    const ParsedUrl& destination,
    bool use_proxy,
    const std::vector<std::string>& headers,
    const std::vector<unsigned char>& body,
    const std::string& content_type) {
    const bool plain_http_proxy = use_proxy && destination.scheme == "http";
    const auto request_target = plain_http_proxy ? original_url : destination.path;
    std::ostringstream request;
    request << method << ' ' << request_target << " HTTP/1.1\r\n";
    request << "Host: " << destination.host;
    const bool non_default_port =
        (destination.scheme == "https" && destination.port != 443) ||
        (destination.scheme == "http" && destination.port != 80);
    if (non_default_port) request << ':' << destination.port;
    request << "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    for (const auto& header : headers) request << header << "\r\n";
    if (!content_type.empty()) request << "Content-Type: " << content_type << "\r\n";
    const bool method_can_have_body =
        method == "POST" || method == "PUT" || method == "PATCH";
    if (!body.empty() || method_can_have_body) {
        request << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    return request.str();
}

HttpResponse parse_response(const std::vector<unsigned char>& bytes) {
    static constexpr unsigned char kHeaderSeparator[] = {'\r', '\n', '\r', '\n'};
    const auto separator = std::search(
        bytes.begin(), bytes.end(), std::begin(kHeaderSeparator), std::end(kHeaderSeparator));
    if (separator == bytes.end()) throw std::runtime_error("Invalid HTTP response");

    const auto header_size = static_cast<std::size_t>(separator - bytes.begin());
    if (header_size > kMaxHeaderBytes) {
        throw std::runtime_error("HTTP headers too large");
    }

    const std::string header_text(
        reinterpret_cast<const char*>(bytes.data()),
        header_size);
    HttpResponse response;
    std::istringstream lines(header_text);
    std::string line;
    if (std::getline(lines, line)) {
        std::istringstream status_line(line);
        std::string protocol;
        status_line >> protocol >> response.status;
        if (protocol.rfind("HTTP/", 0) != 0 || response.status <= 0) {
            throw std::runtime_error("Invalid HTTP status line");
        }
    }
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator_position = line.find(':');
        if (separator_position == std::string::npos) continue;
        response.headers[lowercase(line.substr(0, separator_position))] =
            trim_left(line.substr(separator_position + 1));
    }
    response.body.assign(separator + 4, bytes.end());

    if (const auto transfer_encoding = response.headers.find("transfer-encoding");
        transfer_encoding != response.headers.end() &&
        lowercase(transfer_encoding->second).find("chunked") != std::string::npos) {
        response.body = decode_chunked(response.body);
    } else if (const auto content_length = response.headers.find("content-length");
               content_length != response.headers.end()) {
        try {
            const auto expected = static_cast<std::size_t>(std::stoull(content_length->second));
            if (response.body.size() < expected) {
                throw std::runtime_error("Incomplete HTTP response body");
            }
            if (response.body.size() > expected) response.body.resize(expected);
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("Invalid Content-Length header");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Invalid Content-Length header");
        }
    }
    return response;
}

std::string form_component_encode(const std::string& text) {
    std::ostringstream output;
    output << std::hex << std::uppercase;
    for (const unsigned char character : text) {
        const bool unreserved =
            std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~';
        if (unreserved) {
            output << static_cast<char>(character);
        } else {
            output << '%' << std::setw(2) << std::setfill('0')
                   << static_cast<int>(character);
        }
    }
    return output.str();
}

}  // namespace

HttpClient::HttpClient() {
    std::call_once(g_ssl_once, [] { OPENSSL_init_ssl(0, nullptr); });
}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::vector<unsigned char>& body,
    const std::string& content_type,
    long timeout_seconds,
    std::size_t max_response_bytes) const {
    const auto destination = parse_url(url);
    if (destination.scheme != "https" && destination.scheme != "http") {
        throw std::runtime_error("Only HTTP(S) URLs are supported");
    }

    const bool use_proxy = !proxy_.empty();
    ParsedUrl proxy;
    if (use_proxy) {
        proxy = parse_url(proxy_);
        if (proxy.scheme != "http") {
            throw std::runtime_error("Only http:// proxies are supported");
        }
    }
    const auto endpoint_host = use_proxy ? proxy.host : destination.host;
    const auto endpoint_port = use_proxy ? proxy.port : destination.port;
    const auto endpoint = endpoint_host + ":" + std::to_string(endpoint_port);

    Connection connection;
    connect_tcp(connection, endpoint, timeout_seconds);
    if (use_proxy && destination.scheme == "https") {
        establish_proxy_tunnel(connection, destination);
    }
    const bool use_tls = destination.scheme == "https";
    if (use_tls) enable_tls(connection, destination);

    const auto request_headers = build_request_headers(
        method, url, destination, use_proxy, headers, body, content_type);
    write_all(
        connection,
        use_tls,
        reinterpret_cast<const unsigned char*>(request_headers.data()),
        request_headers.size());
    if (!body.empty()) write_all(connection, use_tls, body.data(), body.size());
    auto response = parse_response(read_all(connection, use_tls, max_response_bytes));
    if (max_response_bytes != 0 && response.body.size() > max_response_bytes) {
        throw std::runtime_error("HTTP response exceeded configured size limit");
    }
    return response;
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::vector<std::string>& headers,
    long timeout_seconds,
    std::size_t max_response_bytes) const {
    return request("GET", url, headers, {}, "", timeout_seconds, max_response_bytes);
}

HttpResponse HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers,
    const std::string& content_type,
    long timeout_seconds) const {
    return request(
        "POST", url, headers,
        std::vector<unsigned char>(body.begin(), body.end()),
        content_type, timeout_seconds);
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
    std::string encoded;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) encoded.push_back('&');
        first = false;
        encoded += form_component_encode(key);
        encoded.push_back('=');
        encoded += form_component_encode(value);
    }
    return encoded;
}

}  // namespace nso
