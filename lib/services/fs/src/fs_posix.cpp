// pd::fs on POSIX (host build). Based on PicoCalc Writer's test/storage_host.cpp.
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "pd/fs.h"

namespace pd::fs {

namespace {
std::string g_root;          // host folder that "/" maps to
bool g_mounted = false;
int g_err = 0;

std::string host(const char* p) {
    if (g_root.empty()) return p;
    if (p[0] == '/') return g_root + p;
    return g_root + "/" + p;
}
bool fail() {
    g_err = errno;
    return false;
}
}  // namespace

int g_fail_read_opens = 0;   // test hook: the next N read-only opens fail with EIO

void set_block_device(hal::BlockDevice*) {}
void set_root(const std::string& dir) {
    g_root = dir;
    while (!g_root.empty() && g_root.back() == '/') g_root.pop_back();
}

int last_error() { return g_err; }
const char* last_error_text() { return g_err ? strerror(g_err) : "no error"; }

File::File() {}
File::~File() { close(); }

bool File::open(const char* path, Mode mode) {
    close();
    if (mode == Mode::Read && g_fail_read_opens > 0) {
        --g_fail_read_opens;
        g_err = EIO;
        return false;
    }
    const char* m = "rb";
    switch (mode) {
        case Mode::Read: m = "rb"; break;
        case Mode::Write: m = "wb"; break;
        case Mode::ReadWrite: m = "w+b"; break;
        case Mode::Append: m = "ab"; break;
        case Mode::Update: m = "r+b"; break;
    }
    FILE* f = fopen(host(path).c_str(), m);
    if (!f) return fail();
    impl_ = f;
    open_ = true;
    return true;
}

void File::close() {
    if (open_) fclose(static_cast<FILE*>(impl_));
    open_ = false;
    impl_ = nullptr;
}

bool File::seek(uint32_t pos) {
    return open_ && (fseek(static_cast<FILE*>(impl_), long(pos), SEEK_SET) == 0 || fail());
}
uint32_t File::tell() { return open_ ? uint32_t(ftell(static_cast<FILE*>(impl_))) : 0; }

int File::read(void* buf, uint32_t n) {
    if (!open_) return -1;
    FILE* f = static_cast<FILE*>(impl_);
    const size_t r = fread(buf, 1, n, f);
    if (r < n && ferror(f)) {
        fail();
        return -1;
    }
    return int(r);
}

int File::write(const void* buf, uint32_t n) {
    if (!open_) return -1;
    const size_t w = fwrite(buf, 1, n, static_cast<FILE*>(impl_));
    if (w < n) fail();
    return int(w);
}

uint32_t File::size() {
    if (!open_) return 0;
    FILE* f = static_cast<FILE*>(impl_);
    const long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    const long s = ftell(f);
    fseek(f, cur, SEEK_SET);
    return uint32_t(s);
}

bool File::sync() { return open_ && (fflush(static_cast<FILE*>(impl_)) == 0 || fail()); }

bool mount() {
    if (!g_root.empty()) ::mkdir(g_root.c_str(), 0755);
    g_mounted = true;
    return true;
}
void unmount() { g_mounted = false; }
bool mounted() { return g_mounted; }

bool space(uint64_t* total, uint64_t* free) {
    struct statvfs s;
    if (statvfs(host("/").c_str(), &s) != 0) return fail();
    if (total) *total = uint64_t(s.f_blocks) * s.f_frsize;
    if (free) *free = uint64_t(s.f_bavail) * s.f_frsize;
    return true;
}

bool stat(const char* path, Info* info) {
    struct stat st;
    if (::stat(host(path).c_str(), &st) != 0) return fail();
    if (info) {
        info->size = uint32_t(st.st_size);
        info->is_dir = S_ISDIR(st.st_mode);
        info->mtime = uint32_t(st.st_mtime);
    }
    return true;
}

bool exists(const char* p) {
    struct stat st;
    return ::stat(host(p).c_str(), &st) == 0;
}
bool is_dir(const char* p) {
    struct stat st;
    return ::stat(host(p).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool remove(const char* p) { return ::remove(host(p).c_str()) == 0 || fail(); }
bool rename(const char* a, const char* b) { return ::rename(host(a).c_str(), host(b).c_str()) == 0 || fail(); }
bool mkdir(const char* p) { return ::mkdir(host(p).c_str(), 0755) == 0 || is_dir(p) || fail(); }

bool copy_metadata(const char* from, const char* to) {
    struct stat st;
    if (::stat(host(from).c_str(), &st) != 0) return fail();
    struct timeval tv[2];
    tv[0].tv_sec = st.st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = st.st_mtime;
    tv[1].tv_usec = 0;
    if (::utimes(host(to).c_str(), tv) != 0) return fail();
    return ::chmod(host(to).c_str(), st.st_mode & 07777) == 0 || fail();
}

bool make_writable(const char* p) {
    struct stat st;
    if (::stat(host(p).c_str(), &st) != 0) return fail();
    return ::chmod(host(p).c_str(), st.st_mode | S_IWUSR) == 0 || fail();
}

bool list_dir(const char* path, std::vector<DirEntry>& out, size_t max_entries, bool include_hidden) {
    DIR* d = opendir(host(path).c_str());
    if (!d) return fail();
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (!include_hidden && n[0] == '.') continue;
        if (out.size() >= max_entries) break;
        struct stat st;
        const std::string full = host(path) + "/" + n;
        if (::stat(full.c_str(), &st) != 0) continue;
        out.push_back(DirEntry{n, S_ISDIR(st.st_mode), uint32_t(st.st_size)});
    }
    closedir(d);
    return true;
}

}  // namespace pd::fs
