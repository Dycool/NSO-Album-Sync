#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace nso {
class Json {
public:
    using object = std::map<std::string, Json>;
    using array = std::vector<Json>;
    using value = std::variant<std::nullptr_t, bool, double, std::string, array, object>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool v) : value_(v) {}
    Json(int v) : value_(static_cast<double>(v)) {}
    Json(int64_t v) : value_(static_cast<double>(v)) {}
    Json(double v) : value_(v) {}
    Json(const char* v) : value_(std::string(v ? v : "")) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(array v) : value_(std::move(v)) {}
    Json(object v) : value_(std::move(v)) {}

    static Json parse(const std::string& text);
    std::string dump() const;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_number() const { return std::holds_alternative<double>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<array>(value_); }
    bool is_object() const { return std::holds_alternative<object>(value_); }

    const object& as_object() const;
    const array& as_array() const;
    const std::string& as_string() const;
    double as_number(double fallback = 0) const;
    int64_t as_i64(int64_t fallback = 0) const;
    bool as_bool(bool fallback = false) const;

    const Json& at(const std::string& key) const;
    const Json* find(const std::string& key) const;
    std::string string(const std::string& key, const std::string& fallback = "") const;
    int64_t integer(const std::string& key, int64_t fallback = 0) const;
    bool boolean(const std::string& key, bool fallback = false) const;

private:
    value value_;
};
}
