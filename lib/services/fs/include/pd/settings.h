// PicoPods - small "key=value" settings file (one pair per line), the format
// Pod Writer uses for /.fwedit/settings.txt. Keys keep their order.
//
//   pd::Settings s;
//   s.load("/.myapp/settings.txt");
//   int volume = s.get_int("volume", 60, 0, 100);
//   s.set_int("volume", volume);
//   s.save("/.myapp/settings.txt");
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pd {

class Settings {
public:
    bool load(const char* path);            // false if missing (settings stay empty)
    bool save(const char* path) const;      // atomic write
    void parse(const std::string& text);
    std::string serialize() const;

    bool has(const char* key) const;
    std::string get(const char* key, const std::string& def = "") const;
    int get_int(const char* key, int def, int min = INT32_MIN, int max = INT32_MAX) const;
    uint32_t get_u32(const char* key, uint32_t def) const;
    bool get_bool(const char* key, bool def) const;

    void set(const char* key, const std::string& value);
    void set_int(const char* key, int value);
    void set_u32(const char* key, uint32_t value);
    void set_bool(const char* key, bool value) { set_int(key, value ? 1 : 0); }
    void remove(const char* key);
    void clear() { kv_.clear(); }

    const std::vector<std::pair<std::string, std::string>>& items() const { return kv_; }

private:
    std::vector<std::pair<std::string, std::string>> kv_;
};

}  // namespace pd
