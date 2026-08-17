#pragma once

#include <optional>
#include <string>

namespace nso {

class SecureStore {
public:
    static bool available();
    static bool put(const std::string& account, const std::string& secret);
    static std::optional<std::string> get(const std::string& account);
    static void erase(const std::string& account);
};

}  // namespace nso
