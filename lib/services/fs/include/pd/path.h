// PicoPods - path string helpers ('/' separated, UTF-8 safe).
#pragma once
#include <string>

namespace pd::path {

std::string join(const std::string& dir, const std::string& name);   // "/a" + "b" -> "/a/b"
std::string parent(const std::string& path);                          // "/a/b" -> "/a", "/a" -> "/"
std::string base_name(const std::string& path);                       // "/a/b.txt" -> "b.txt"
std::string stem(const std::string& path);                            // "/a/b.txt" -> "b"
std::string extension(const std::string& path);                      // "/a/b.TXT" -> ".txt" (lower case)
bool has_extension(const std::string& path, const char* ext);         // case-insensitive, ext with dot
// Replaces (or adds) the extension: ("/r/game.gb", ".sav") -> "/r/game.sav"
std::string with_extension(const std::string& path, const char* ext);

}  // namespace pd::path
