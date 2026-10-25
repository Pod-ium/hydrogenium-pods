// Backend-independent parts of pd::fs, pd::path and pd::Settings.
#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pd/fs.h"
#include "pd/path.h"
#include "pd/settings.h"

namespace pd::fs {

bool mkdirs(const char* p) {
    const std::string s = p;
    for (size_t i = 1; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            const std::string part = s.substr(0, i);
            if (!is_dir(part.c_str()) && !mkdir(part.c_str())) return false;
        }
    }
    return true;
}

bool read_file(const char* p, std::string& out, size_t max_bytes) {
    File f;
    out.clear();
    if (!f.open(p, Mode::Read)) return false;
    const uint32_t size = f.size();
    if (size > max_bytes) return false;
    out.resize(size);
    return size == 0 || f.read(&out[0], size) == int(size);
}

bool write_file(const char* p, const void* data, size_t n, bool atomic) {
    const std::string target = p;
    const std::string tmp = atomic ? target + ".tmp" : target;
    {
        File f;
        if (!f.open(tmp.c_str(), Mode::Write)) return false;
        if (n && f.write(data, uint32_t(n)) != int(n)) {
            f.close();
            if (atomic) remove(tmp.c_str());
            return false;
        }
        if (!f.sync()) return false;
    }
    if (!atomic) return true;
    if (exists(p)) remove(p);   // FatFs rename does not replace
    return rename(tmp.c_str(), p);
}

}  // namespace pd::fs

namespace pd::path {

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string parent(const std::string& p) {
    const size_t i = p.rfind('/');
    if (i == std::string::npos || i == 0) return "/";
    return p.substr(0, i);
}

std::string base_name(const std::string& p) {
    const size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string stem(const std::string& p) {
    const std::string b = base_name(p);
    const size_t dot = b.rfind('.');
    return (dot == std::string::npos || dot == 0) ? b : b.substr(0, dot);
}

std::string extension(const std::string& p) {
    const std::string b = base_name(p);
    const size_t dot = b.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string e = b.substr(dot);
    for (char& c : e)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return e;
}

bool has_extension(const std::string& p, const char* ext) { return strcasecmp(extension(p).c_str(), ext) == 0; }

std::string with_extension(const std::string& p, const char* ext) {
    const std::string dir_part = p.substr(0, p.size() - base_name(p).size());
    return dir_part + stem(p) + ext;
}

}  // namespace pd::path

namespace pd {

bool Settings::load(const char* p) {
    std::string text;
    if (!fs::read_file(p, text, 64 * 1024)) return false;
    parse(text);
    return true;
}

bool Settings::save(const char* p) const { return fs::write_file(p, serialize()); }

void Settings::parse(const std::string& text) {
    kv_.clear();
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        set(line.substr(0, eq).c_str(), line.substr(eq + 1));
    }
}

std::string Settings::serialize() const {
    std::string s;
    for (const auto& kv : kv_) s += kv.first + "=" + kv.second + "\n";
    return s;
}

bool Settings::has(const char* key) const {
    for (const auto& kv : kv_)
        if (kv.first == key) return true;
    return false;
}

std::string Settings::get(const char* key, const std::string& def) const {
    for (const auto& kv : kv_)
        if (kv.first == key) return kv.second;
    return def;
}

int Settings::get_int(const char* key, int def, int min, int max) const {
    if (!has(key)) return def;
    const std::string v = get(key);
    char* end = nullptr;
    const long n = strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return def;
    if (n < min) return min;
    if (n > max) return max;
    return int(n);
}

uint32_t Settings::get_u32(const char* key, uint32_t def) const {
    if (!has(key)) return def;
    const std::string v = get(key);
    char* end = nullptr;
    const unsigned long n = strtoul(v.c_str(), &end, 10);
    return end == v.c_str() ? def : uint32_t(n);
}

bool Settings::get_bool(const char* key, bool def) const {
    if (!has(key)) return def;
    const std::string v = get(key);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void Settings::set(const char* key, const std::string& value) {
    std::string v = value;
    for (char& c : v)
        if (c == '\n' || c == '\r') c = ' ';   // one line per value
    for (auto& kv : kv_)
        if (kv.first == key) {
            kv.second = v;
            return;
        }
    kv_.emplace_back(key, v);
}

void Settings::set_int(const char* key, int value) { set(key, std::to_string(value)); }
void Settings::set_u32(const char* key, uint32_t value) { set(key, std::to_string(value)); }

void Settings::remove(const char* key) {
    for (auto it = kv_.begin(); it != kv_.end(); ++it)
        if (it->first == key) {
            kv_.erase(it);
            return;
        }
}

}  // namespace pd
