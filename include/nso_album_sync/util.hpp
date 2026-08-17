#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nso {

std::string base64_encode(const std::vector<unsigned char>& data);
std::vector<unsigned char> base64_decode(std::string text);
std::string base64url(const std::vector<unsigned char>& data);

std::vector<unsigned char> random_bytes(std::size_t count);
std::vector<unsigned char> sha256(const std::string& text);

std::string url_encode(const std::string& text);
std::string lower(std::string text);
std::string trim(std::string text);
std::string sanitize_folder(std::string text);
std::string normalize_title(std::string text);

std::int64_t unix_now();

void open_url(const std::string& url);
void open_path(const std::filesystem::path& path);

}  // namespace nso
