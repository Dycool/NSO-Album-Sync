#pragma once

#include <optional>
#include <string>

namespace nso {

inline constexpr char kNintendoAuthScheme[] = "npf71b963c1b7b6d119";
inline constexpr char kNintendoAuthCallbackPrefix[] = "npf71b963c1b7b6d119://auth";

bool is_nintendo_auth_callback(const std::string& url);
bool register_nintendo_auth_protocol();
void unregister_nintendo_auth_protocol();
bool publish_nintendo_auth_callback(const std::string& url);
std::optional<std::string> take_nintendo_auth_callback();
void clear_nintendo_auth_callback();

#ifdef _WIN32
bool forward_nintendo_auth_callback_to_running_instance(const std::string& url);
#endif

}  // namespace nso
