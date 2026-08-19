#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nso {

inline constexpr std::size_t kDefaultGetResponseLimit =
    256ULL * 1024ULL * 1024ULL;

// Response headers are map-like for existing callers, but Set-Cookie is the
// one HTTP response field that cannot be safely collapsed. The OpenSSL
// transport feeds headers through operator[]; Windows can also assign its
// already-parsed map directly. In both cases every Nintendo session cookie is
// retained while ordinary duplicate headers keep normal last-value semantics.
class HttpResponseHeaders {
public:
    using Storage = std::map<std::string, std::string>;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    class ValueProxy {
    public:
        ValueProxy(HttpResponseHeaders& owner, std::string key)
            : owner_(owner), key_(std::move(key)) {}

        ValueProxy& operator=(std::string value) {
            owner_.assign(key_, std::move(value));
            return *this;
        }

        ValueProxy& operator=(const char* value) {
            owner_.assign(key_, value == nullptr ? std::string{} : std::string(value));
            return *this;
        }

        operator const std::string&() const {
            return owner_.value(key_);
        }

    private:
        HttpResponseHeaders& owner_;
        std::string key_;
    };

    HttpResponseHeaders() = default;
    HttpResponseHeaders(const HttpResponseHeaders&) = default;
    HttpResponseHeaders(HttpResponseHeaders&&) noexcept = default;
    HttpResponseHeaders& operator=(const HttpResponseHeaders&) = default;
    HttpResponseHeaders& operator=(HttpResponseHeaders&&) noexcept = default;

    HttpResponseHeaders& operator=(Storage values) {
        values_ = std::move(values);
        return *this;
    }

    operator const Storage&() const { return values_; }
    operator Storage&() { return values_; }

    ValueProxy operator[](std::string key) {
        return ValueProxy(*this, std::move(key));
    }

    const std::string& operator[](const std::string& key) const {
        return value(key);
    }

    iterator find(const std::string& key) { return values_.find(key); }
    const_iterator find(const std::string& key) const { return values_.find(key); }
    iterator begin() { return values_.begin(); }
    const_iterator begin() const { return values_.begin(); }
    const_iterator cbegin() const { return values_.cbegin(); }
    iterator end() { return values_.end(); }
    const_iterator end() const { return values_.end(); }
    const_iterator cend() const { return values_.cend(); }
    bool empty() const { return values_.empty(); }
    std::size_t size() const { return values_.size(); }
    void clear() { values_.clear(); }

private:
    void assign(const std::string& key, std::string value) {
        auto it = values_.find(key);
        if (key == "set-cookie" && it != values_.end() && !it->second.empty()) {
            it->second.push_back('\n');
            it->second += value;
            return;
        }
        values_[key] = std::move(value);
    }

    const std::string& value(const std::string& key) const {
        static const std::string empty_value;
        const auto it = values_.find(key);
        return it == values_.end() ? empty_value : it->second;
    }

    Storage values_;
};

struct HttpResponse {
    long status = 0;
    std::vector<unsigned char> body;
    HttpResponseHeaders headers;

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
        long timeout_seconds = 30,
        std::size_t max_response_bytes = 0) const;

    HttpResponse get(
        const std::string& url,
        const std::vector<std::string>& headers = {},
        long timeout_seconds = 30,
        std::size_t max_response_bytes = kDefaultGetResponseLimit) const;

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
    class SynchronizedString {
    public:
        SynchronizedString() = default;
        SynchronizedString(const SynchronizedString&) = delete;
        SynchronizedString& operator=(const SynchronizedString&) = delete;

        SynchronizedString& operator=(std::string value) {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            return *this;
        }

        bool empty() const {
            std::lock_guard lock(mutex_);
            return value_.empty();
        }

        operator std::string() const {
            std::lock_guard lock(mutex_);
            return value_;
        }

    private:
        mutable std::mutex mutex_;
        std::string value_;
    };

    SynchronizedString proxy_;
};

}  // namespace nso
