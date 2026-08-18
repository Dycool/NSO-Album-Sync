#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace nso {

inline constexpr char kNintendoAuthScheme[] = "npf71b963c1b7b6d119";
inline constexpr char kNintendoAuthCallbackPrefix[] = "npf71b963c1b7b6d119://auth";

// Private per-user runtime directory used for short-lived callback IPC and the
// Unix single-instance lock. On POSIX this directory is created mode 0700 and
// validated as owned by the current user before any sensitive file is opened.
std::filesystem::path application_runtime_directory();

bool is_nintendo_auth_callback(const std::string& url);
bool register_nintendo_auth_protocol();
void unregister_nintendo_auth_protocol();
bool publish_nintendo_auth_callback(const std::string& url);
std::optional<std::string> take_nintendo_auth_callback();
void clear_nintendo_auth_callback();

}  // namespace nso
