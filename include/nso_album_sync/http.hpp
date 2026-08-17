#pragma once

#include <map>
#include <string>
#include <vector>

namespace nso {

struct HttpResponse {
    long status = 0;
    std::vector<unsigned char> body;
    std::map<std::string, std::string> headers;

    std::string text() const {
        return std::string(body.begin(), body.end());
    }
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    void set_proxy(std::string proxy) { proxy_ = std::move(proxy); }

    HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers = {},
        const std::vector<unsigned char>& body = {},
        const std::string& content_type = "",
        long timeout_seconds = 30) const;

    HttpResponse get(
        const std::string& url,
        const std::vector<std::string>& headers = {},
        long timeout_seconds = 30) const;

    HttpResponse post(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers = {},
        const std::string& content_type = "application/json",
        long timeout_seconds = 30) const;

    HttpResponse post_bytes(
        const std::string& url,
        const std::vector<unsigned char>& body,
        const std::vector<std::string>& headers = {},
        const std::string& content_type = "application/octet-stream",
        long timeout_seconds = 30) const;

    static std::string form_encode(
        const std::map<std::string, std::string>& values);

private:
    std::string proxy_;
};

}  // namespace nso
