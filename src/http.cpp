#include "nso_album_sync/http.hpp"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

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
        if (io != nullptr && io != raw) {
            BIO_free(io);
        }
        if (ssl != nullptr) {
            SSL_free(ssl);
        }
        if (context != nullptr) {
            SSL_CTX_free(context);
        }
        if (raw != nullptr) {
            BIO_free_all(raw);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection() = default;
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

    parsed.path = path_start == std::string::npos
        ? "/"
        : url.substr(path_start);

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
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
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

std::vector<unsigned char> decode_chunked(
    const std::vector<unsigned char>& encoded) {
    std::vector<unsigned char> decoded;
    std::size_t position = 0;

    while (position < encoded.size()) {
        auto line_end = position;
        while (line_end + 1 < encoded.size() &&
               !(encoded[line_end] == '\r' && encoded[line_end + 1] == '\n')) {
            ++line_end;
        }

        if (line_end + 1 >= encoded.size()) {
            break;
        }

        std::string length_text(
            reinterpret_cast<const char*>(encoded.data()) + position,
            line_end - position);

        if (const auto extension = length_text.find(';');
            extension != std::string::npos) {
            length_text.resize(extension);
        }

        std::size_t chunk_length = 0;
        std::stringstream parser;
        parser << std::hex << length_text;
        parser >> chunk_length;

        position = line_end + 2;

        if (chunk_length == 0) {
            break;
        }

        if (position + chunk_length > encoded.size()) {
            throw std::runtime_error("Invalid chunked HTTP response");
        }

        decoded.insert(
            decoded.end(),
            encoded.begin() + static_cast<std::ptrdiff_t>(position),
            encoded.begin() + static_cast<std::ptrdiff_t>(position + chunk_length));

        position += chunk_length;

        if (position + 1 < encoded.size() &&
            encoded[position] == '\r' &&
            encoded[position + 1] == '\n') {
            position += 2;
        }
    }

    return decoded;
}

std::string read_until(BIO* bio, const std::string& marker) {
    std::string result;
    char buffer[1024];

    while (result.find(marker) == std::string::npos) {
        const int bytes_read = BIO_read(bio, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            throw std::runtime_error(
                "Connection closed while reading HTTP headers");
        }

        result.append(buffer, bytes_read);
        if (result.size() > kMaxHeaderBytes) {
            throw std::runtime_error("HTTP headers too large");
        }
    }

    return result;
}

void connect_tcp(Connection& connection, const std::string& endpoint) {
    connection.raw = BIO_new_connect(endpoint.c_str());
    if (connection.raw == nullptr || BIO_do_connect(connection.raw) <= 0) {
        throw std::runtime_error("TCP connection failed");
    }
    connection.io = connection.raw;
}

void establish_proxy_tunnel(
    Connection& connection,
    const ParsedUrl& destination) {
    const auto authority =
        destination.host + ":" + std::to_string(destination.port);

    const std::string request =
        "CONNECT " + authority + " HTTP/1.1\r\n" +
        "Host: " + authority + "\r\n" +
        "Proxy-Connection: Keep-Alive\r\n\r\n";

    if (BIO_write(
            connection.raw,
            request.data(),
            static_cast<int>(request.size())) !=
        static_cast<int>(request.size())) {
        throw std::runtime_error("Proxy CONNECT write failed");
    }

    const auto response_headers = read_until(connection.raw, "\r\n\r\n");
    const bool proxy_accepted =
        response_headers.rfind("HTTP/1.1 200", 0) == 0 ||
        response_headers.rfind("HTTP/1.0 200", 0) == 0 ||
        response_headers.find(" 200 ") != std::string::npos;

    if (!proxy_accepted) {
        throw std::runtime_error("HTTP proxy CONNECT failed");
    }
}

void enable_tls(Connection& connection, const ParsedUrl& destination) {
    connection.context = SSL_CTX_new(TLS_client_method());
    if (connection.context == nullptr) {
        throw std::runtime_error("SSL_CTX_new failed");
    }

    SSL_CTX_set_verify(connection.context, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(connection.context);

    connection.ssl = SSL_new(connection.context);
    if (connection.ssl == nullptr) {
        throw std::runtime_error("SSL_new failed");
    }

    SSL_set_tlsext_host_name(connection.ssl, destination.host.c_str());
    SSL_set1_host(connection.ssl, destination.host.c_str());

    // SSL takes ownership of the connected BIO after SSL_set_bio().
    SSL_set_bio(connection.ssl, connection.raw, connection.raw);
    connection.raw = nullptr;
    connection.io = nullptr;

    if (SSL_connect(connection.ssl) != 1) {
        throw std::runtime_error("TLS handshake failed");
    }

    connection.io = BIO_new(BIO_f_ssl());
    if (connection.io == nullptr) {
        throw std::runtime_error("BIO_new(BIO_f_ssl()) failed");
    }

    BIO_set_ssl(connection.io, connection.ssl, BIO_NOCLOSE);
}

void write_all(
    Connection& connection,
    bool use_tls,
    const unsigned char* data,
    std::size_t length) {
    while (length > 0) {
        const int written = use_tls
            ? SSL_write(connection.ssl, data, static_cast<int>(length))
            : BIO_write(connection.io, data, static_cast<int>(length));

        if (written <= 0) {
            throw std::runtime_error("HTTP write failed");
        }

        data += written;
        length -= static_cast<std::size_t>(written);
    }
}

std::vector<unsigned char> read_all(Connection& connection, bool use_tls) {
    std::vector<unsigned char> response;
    unsigned char buffer[kReadBufferBytes];

    for (;;) {
        const int bytes_read = use_tls
            ? SSL_read(connection.ssl, buffer, sizeof(buffer))
            : BIO_read(connection.io, buffer, sizeof(buffer));

        if (bytes_read <= 0) {
            break;
        }

        response.insert(response.end(), buffer, buffer + bytes_read);
    }

    return response;
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
    if (non_default_port) {
        request << ':' << destination.port;
    }

    request << "\r\n";
    request << "Connection: close\r\n";
    request << "Accept-Encoding: identity\r\n";

    for (const auto& header : headers) {
        request << header << "\r\n";
    }

    if (!content_type.empty()) {
        request << "Content-Type: " << content_type << "\r\n";
    }

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
        bytes.begin(),
        bytes.end(),
        std::begin(kHeaderSeparator),
        std::end(kHeaderSeparator));

    if (separator == bytes.end()) {
        throw std::runtime_error("Invalid HTTP response");
    }

    const auto header_bytes =
        static_cast<std::size_t>(separator - bytes.begin());
    const std::string header_text(
        reinterpret_cast<const char*>(bytes.data()),
        header_bytes);

    HttpResponse response;
    std::istringstream lines(header_text);
    std::string line;

    if (std::getline(lines, line)) {
        std::istringstream status_line(line);
        std::string protocol;
        status_line >> protocol >> response.status;
    }

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto separator_position = line.find(':');
        if (separator_position == std::string::npos) {
            continue;
        }

        const auto key = lowercase(line.substr(0, separator_position));
        const auto value = trim_left(line.substr(separator_position + 1));
        response.headers[key] = value;
    }

    response.body.assign(separator + 4, bytes.end());

    if (const auto transfer_encoding =
            response.headers.find("transfer-encoding");
        transfer_encoding != response.headers.end() &&
        lowercase(transfer_encoding->second).find("chunked") != std::string::npos) {
        response.body = decode_chunked(response.body);
    }

    return response;
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

HttpClient::HttpClient() {
    std::call_once(g_ssl_once, [] {
        OPENSSL_init_ssl(0, nullptr);
    });
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::vector<unsigned char>& body,
    const std::string& content_type,
    long timeout_seconds) const {
    // The compact BIO implementation currently relies on the OS socket timeout.
    // Keep the API parameter so callers can move to explicit socket timeouts later.
    (void)timeout_seconds;

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
    connect_tcp(connection, endpoint);

    if (use_proxy && destination.scheme == "https") {
        establish_proxy_tunnel(connection, destination);
    }

    const bool use_tls = destination.scheme == "https";
    if (use_tls) {
        enable_tls(connection, destination);
    }

    const auto request_headers = build_request_headers(
        method,
        url,
        destination,
        use_proxy,
        headers,
        body,
        content_type);

    write_all(
        connection,
        use_tls,
        reinterpret_cast<const unsigned char*>(request_headers.data()),
        request_headers.size());

    if (!body.empty()) {
        write_all(connection, use_tls, body.data(), body.size());
    }

    return parse_response(read_all(connection, use_tls));
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
    return request(
        "POST",
        url,
        headers,
        body,
        content_type,
        timeout_seconds);
}

std::string HttpClient::form_encode(
    const std::map<std::string, std::string>& values) {
    std::string encoded;
    bool first = true;

    for (const auto& [key, value] : values) {
        if (!first) {
            encoded.push_back('&');
        }
        first = false;

        encoded += form_component_encode(key);
        encoded.push_back('=');
        encoded += form_component_encode(value);
    }

    return encoded;
}

}  // namespace nso
