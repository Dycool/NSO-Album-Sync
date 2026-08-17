#include "nso_album_sync/json.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace nso {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse() {
        skip_whitespace();
        auto result = parse_value();
        skip_whitespace();

        if (position_ != text_.size()) {
            fail("trailing input");
        }

        return result;
    }

private:
    const std::string& text_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(const char* reason) const {
        throw std::runtime_error(
            std::string("JSON ") + reason +
            " at byte " + std::to_string(position_));
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' &&
                character != '\n' &&
                character != '\r' &&
                character != '\t') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        skip_whitespace();

        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }

        return false;
    }

    Json parse_value() {
        skip_whitespace();

        if (position_ >= text_.size()) {
            fail("unexpected eof");
        }

        const char character = text_[position_];

        switch (character) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
                return Json(parse_string());
            case 't':
                parse_literal("true");
                return Json(true);
            case 'f':
                parse_literal("false");
                return Json(false);
            case 'n':
                parse_literal("null");
                return Json(nullptr);
            default:
                if (character == '-' ||
                    (character >= '0' && character <= '9')) {
                    return parse_number();
                }
                fail("invalid value");
        }
    }

    void parse_literal(const char* literal) {
        while (*literal != '\0') {
            if (position_ >= text_.size() || text_[position_] != *literal) {
                fail("invalid literal");
            }

            ++position_;
            ++literal;
        }
    }

    Json parse_object() {
        ++position_;  // '{'
        Json::object object;

        skip_whitespace();
        if (consume('}')) {
            return Json(std::move(object));
        }

        for (;;) {
            skip_whitespace();

            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("expected object key");
            }

            auto key = parse_string();
            if (!consume(':')) {
                fail("expected colon");
            }

            object.emplace(std::move(key), parse_value());

            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                fail("expected comma");
            }
        }

        return Json(std::move(object));
    }

    Json parse_array() {
        ++position_;  // '['
        Json::array array;

        skip_whitespace();
        if (consume(']')) {
            return Json(std::move(array));
        }

        for (;;) {
            array.emplace_back(parse_value());

            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                fail("expected comma");
            }
        }

        return Json(std::move(array));
    }

    static void append_utf8(std::string& output, unsigned code_point) {
        if (code_point <= 0x7F) {
            output.push_back(static_cast<char>(code_point));
            return;
        }

        if (code_point <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            return;
        }

        if (code_point <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            return;
        }

        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(
            static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(
            static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }

    unsigned parse_hex4() {
        unsigned value = 0;

        for (int digit = 0; digit < 4; ++digit) {
            if (position_ >= text_.size()) {
                fail("unicode escape");
            }

            const char character = text_[position_++];
            value <<= 4;

            if (character >= '0' && character <= '9') {
                value |= static_cast<unsigned>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<unsigned>(10 + character - 'a');
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<unsigned>(10 + character - 'A');
            } else {
                fail("unicode escape");
            }
        }

        return value;
    }

    std::string parse_string() {
        if (text_[position_++] != '"') {
            fail("expected string");
        }

        std::string output;

        while (position_ < text_.size()) {
            const char character = text_[position_++];

            if (character == '"') {
                return output;
            }

            if (static_cast<unsigned char>(character) < 0x20) {
                fail("control in string");
            }

            if (character != '\\') {
                output.push_back(character);
                continue;
            }

            if (position_ >= text_.size()) {
                fail("escape");
            }

            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                    output.push_back('"');
                    break;
                case '\\':
                    output.push_back('\\');
                    break;
                case '/':
                    output.push_back('/');
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    unsigned code_point = parse_hex4();

                    const bool high_surrogate =
                        code_point >= 0xD800 && code_point <= 0xDBFF;
                    const bool has_second_escape =
                        position_ + 1 < text_.size() &&
                        text_[position_] == '\\' &&
                        text_[position_ + 1] == 'u';

                    if (high_surrogate && has_second_escape) {
                        position_ += 2;
                        const unsigned low_surrogate = parse_hex4();

                        if (low_surrogate >= 0xDC00 && low_surrogate <= 0xDFFF) {
                            code_point =
                                0x10000 +
                                ((code_point - 0xD800) << 10) +
                                (low_surrogate - 0xDC00);
                        }
                    }

                    append_utf8(output, code_point);
                    break;
                }
                default:
                    fail("escape");
            }
        }

        fail("unterminated string");
    }

    Json parse_number() {
        const auto start = position_;

        if (text_[position_] == '-') {
            ++position_;
        }

        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }

        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }

        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;

            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }

            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }

        char* parsed_end = nullptr;
        const double value = std::strtod(text_.c_str() + start, &parsed_end);

        if (parsed_end != text_.c_str() + position_) {
            fail("number");
        }

        return Json(value);
    }
};

std::string escape_string(const std::string& text) {
    std::ostringstream output;
    output << '"';

    for (const unsigned char character : text) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u"
                           << std::hex
                           << std::setw(4)
                           << std::setfill('0')
                           << static_cast<int>(character)
                           << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
                break;
        }
    }

    output << '"';
    return output.str();
}

}  // namespace

Json Json::parse(const std::string& text) {
    return Parser(text).parse();
}

const Json::object& Json::as_object() const {
    if (!is_object()) {
        throw std::runtime_error("JSON value is not object");
    }
    return std::get<object>(value_);
}

const Json::array& Json::as_array() const {
    if (!is_array()) {
        throw std::runtime_error("JSON value is not array");
    }
    return std::get<array>(value_);
}

const std::string& Json::as_string() const {
    if (!is_string()) {
        throw std::runtime_error("JSON value is not string");
    }
    return std::get<std::string>(value_);
}

double Json::as_number(double fallback) const {
    return is_number() ? std::get<double>(value_) : fallback;
}

std::int64_t Json::as_i64(std::int64_t fallback) const {
    return is_number()
        ? static_cast<std::int64_t>(std::get<double>(value_))
        : fallback;
}

bool Json::as_bool(bool fallback) const {
    return is_bool() ? std::get<bool>(value_) : fallback;
}

const Json& Json::at(const std::string& key) const {
    return as_object().at(key);
}

const Json* Json::find(const std::string& key) const {
    if (!is_object()) {
        return nullptr;
    }

    const auto& object = std::get<Json::object>(value_);
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

std::string Json::string(
    const std::string& key,
    const std::string& fallback) const {
    const auto* value = find(key);
    if (value == nullptr) {
        return fallback;
    }

    if (value->is_string()) {
        return value->as_string();
    }

    if (value->is_number()) {
        std::ostringstream output;
        output << std::fixed << std::setprecision(0) << value->as_number();
        return output.str();
    }

    return fallback;
}

std::int64_t Json::integer(
    const std::string& key,
    std::int64_t fallback) const {
    const auto* value = find(key);
    return value != nullptr ? value->as_i64(fallback) : fallback;
}

bool Json::boolean(const std::string& key, bool fallback) const {
    const auto* value = find(key);
    return value != nullptr ? value->as_bool(fallback) : fallback;
}

std::string Json::dump() const {
    if (is_null()) {
        return "null";
    }

    if (is_bool()) {
        return as_bool() ? "true" : "false";
    }

    if (is_number()) {
        const double number = as_number();
        std::ostringstream output;

        if (std::floor(number) == number) {
            output << std::fixed << std::setprecision(0) << number;
        } else {
            output << std::setprecision(17) << number;
        }

        return output.str();
    }

    if (is_string()) {
        return escape_string(as_string());
    }

    if (is_array()) {
        std::string output = "[";
        bool first = true;

        for (const auto& value : as_array()) {
            if (!first) {
                output += ',';
            }
            first = false;
            output += value.dump();
        }

        output += ']';
        return output;
    }

    std::string output = "{";
    bool first = true;

    for (const auto& [key, value] : as_object()) {
        if (!first) {
            output += ',';
        }
        first = false;

        output += escape_string(key);
        output += ':';
        output += value.dump();
    }

    output += '}';
    return output;
}

}  // namespace nso
