// PicoPods - flash cache on the host: the file is kept in RAM.
#include <cstdlib>
#include <cstring>
#include <string>

#include "pd/flash_cache.h"
#include "pd/fs.h"

namespace pd::flash_cache {
namespace {
uint32_t g_cap = 0;
uint8_t* g_data = nullptr;
std::string g_path;
uint32_t g_size = 0, g_mtime = 0;

const uint8_t* fail(const char** err, const char* m) {
    if (err) *err = m;
    return nullptr;
}
}  // namespace

bool init(uint32_t max_bytes) {
    g_cap = max_bytes;
    return true;
}
uint32_t capacity() { return g_cap; }
void invalidate() { g_path.clear(); }

const uint8_t* load(const char* path, uint32_t* size_out, const Progress& progress, const char** err) {
    if (err) *err = nullptr;
    fs::Info info;
    if (!fs::stat(path, &info) || info.is_dir) return fail(err, "Cannot read file");
    if (info.size == 0) return fail(err, "File is empty");
    if (info.size > g_cap) return fail(err, "File too large for the flash cache");
    if (g_data && g_path == path && g_size == info.size && g_mtime == info.mtime) {
        if (size_out) *size_out = g_size;
        return g_data;
    }
    free(g_data);
    g_data = static_cast<uint8_t*>(malloc(info.size));
    fs::File f;
    if (!g_data || !f.open(path, fs::Mode::Read)) return fail(err, "Cannot open file");
    for (uint32_t off = 0; off < info.size; off += 4096) {
        const uint32_t n = info.size - off < 4096 ? info.size - off : 4096;
        if (f.read(g_data + off, n) != int(n)) return fail(err, "Read error");
        if (progress && !progress(off + n, info.size)) return fail(err, "Cancelled");
    }
    g_path = path;
    g_size = info.size;
    g_mtime = info.mtime;
    if (size_out) *size_out = g_size;
    return g_data;
}

}  // namespace pd::flash_cache
