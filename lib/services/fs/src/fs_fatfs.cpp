// pd::fs on FatFs (device build). Based on PicoCalc Writer's storage_fatfs.cpp.
#include <cstring>

#include "pd/fs.h"
#include "pd/hal/block_device.h"

extern "C" {
#include "ff.h"
}

namespace pd::fs {

namespace {
FATFS g_fs;
bool g_mounted = false;
int g_err = 0;

inline bool check(FRESULT r) {
    if (r != FR_OK) g_err = int(r);
    return r == FR_OK;
}
}  // namespace

hal::BlockDevice* g_block_device = nullptr;   // used by diskio.cpp

void set_block_device(hal::BlockDevice* dev) { g_block_device = dev; }
void set_root(const std::string&) {}

int last_error() { return g_err; }

const char* last_error_text() {
    switch (g_err) {
        case FR_OK: return "no error";
        case FR_DISK_ERR: return "SD card I/O error";
        case FR_INT_ERR: return "file system error";
        case FR_NOT_READY: return "SD card not ready";
        case FR_NO_FILE: return "file not found";
        case FR_NO_PATH: return "folder not found";
        case FR_INVALID_NAME: return "invalid name";
        case FR_DENIED: return "access denied or disk full";
        case FR_EXIST: return "already exists";
        case FR_WRITE_PROTECTED: return "write protected";
        case FR_NOT_ENABLED:
        case FR_NO_FILESYSTEM: return "no file system";
        case FR_TIMEOUT: return "timeout";
        case FR_LOCKED: return "file locked";
        case FR_NOT_ENOUGH_CORE: return "out of memory";
        case FR_TOO_MANY_OPEN_FILES: return "too many open files";
        default: return "error";
    }
}

// ---------------------------------------------------------------- File
File::File() {}
File::~File() { close(); }

bool File::open(const char* path, Mode mode) {
    close();
    BYTE m = FA_READ | FA_OPEN_EXISTING;
    switch (mode) {
        case Mode::Read: m = FA_READ | FA_OPEN_EXISTING; break;
        case Mode::Write: m = FA_WRITE | FA_CREATE_ALWAYS; break;
        case Mode::ReadWrite: m = FA_READ | FA_WRITE | FA_CREATE_ALWAYS; break;
        case Mode::Append: m = FA_WRITE | FA_OPEN_APPEND; break;
        case Mode::Update: m = FA_READ | FA_WRITE | FA_OPEN_EXISTING; break;
    }
    FIL* f = new FIL;
    if (!check(f_open(f, path, m))) {
        delete f;
        return false;
    }
    impl_ = f;
    open_ = true;
    return true;
}

void File::close() {
    if (open_) {
        FIL* f = static_cast<FIL*>(impl_);
        f_close(f);
        delete f;
    }
    open_ = false;
    impl_ = nullptr;
}

bool File::seek(uint32_t pos) { return open_ && check(f_lseek(static_cast<FIL*>(impl_), pos)); }
uint32_t File::tell() { return open_ ? uint32_t(f_tell(static_cast<FIL*>(impl_))) : 0; }

int File::read(void* buf, uint32_t n) {
    if (!open_) return -1;
    UINT br = 0;
    if (!check(f_read(static_cast<FIL*>(impl_), buf, n, &br))) return -1;
    return int(br);
}

int File::write(const void* buf, uint32_t n) {
    if (!open_) return -1;
    UINT bw = 0;
    if (!check(f_write(static_cast<FIL*>(impl_), buf, n, &bw))) return -1;
    return int(bw);
}

uint32_t File::size() { return open_ ? uint32_t(f_size(static_cast<FIL*>(impl_))) : 0; }
bool File::sync() { return open_ && check(f_sync(static_cast<FIL*>(impl_))); }

// ---------------------------------------------------------------- volume
bool mount() {
    if (!g_block_device) {
        g_err = FR_NOT_READY;
        return false;
    }
    g_mounted = check(f_mount(&g_fs, "", 1));
    return g_mounted;
}

void unmount() {
    f_mount(nullptr, "", 0);
    g_mounted = false;
}

bool mounted() { return g_mounted; }

bool space(uint64_t* total, uint64_t* free) {
    FATFS* fs = nullptr;
    DWORD clusters = 0;
    if (!check(f_getfree("", &clusters, &fs))) return false;
    const uint64_t csize = uint64_t(fs->csize) * 512u;
    if (total) *total = uint64_t(fs->n_fatent - 2) * csize;
    if (free) *free = uint64_t(clusters) * csize;
    return true;
}

// ---------------------------------------------------------------- paths
bool stat(const char* path, Info* info) {
    if (path[0] == '/' && path[1] == 0) {
        if (info) *info = Info{0, true, 0};
        return true;
    }
    FILINFO fi;
    if (!check(f_stat(path, &fi))) return false;
    if (info) {
        info->size = uint32_t(fi.fsize);
        info->is_dir = (fi.fattrib & AM_DIR) != 0;
        info->mtime = (uint32_t(fi.fdate) << 16) | fi.ftime;
    }
    return true;
}

bool exists(const char* path) {
    if (path[0] == '/' && path[1] == 0) return true;
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

bool is_dir(const char* path) {
    Info i;
    const int saved = g_err;
    const bool ok = stat(path, &i) && i.is_dir;
    g_err = saved;
    return ok;
}

bool remove(const char* path) { return check(f_unlink(path)); }
bool rename(const char* from, const char* to) { return check(f_rename(from, to)); }

bool mkdir(const char* path) {
    const FRESULT r = f_mkdir(path);
    if (r == FR_OK || r == FR_EXIST) return true;
    g_err = int(r);
    return false;
}

bool copy_metadata(const char* from, const char* to) {
    FILINFO fi;
    if (!check(f_stat(from, &fi))) return false;
    const BYTE mask = AM_RDO | AM_HID | AM_SYS | AM_ARC;
    // Timestamp first: f_utime on a read-only file is allowed, but keep the
    // order obvious.
    if (!check(f_utime(to, &fi))) return false;
    return check(f_chmod(to, fi.fattrib & mask, mask));
}

bool make_writable(const char* path) { return check(f_chmod(path, 0, AM_RDO)); }

bool list_dir(const char* path, std::vector<DirEntry>& out, size_t max_entries, bool include_hidden) {
    DIR dir;
    if (!check(f_opendir(&dir, path))) return false;
    FILINFO fi;
    while (out.size() < max_entries && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!include_hidden && (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)))) continue;
        out.push_back(DirEntry{fi.fname, (fi.fattrib & AM_DIR) != 0, uint32_t(fi.fsize)});
    }
    f_closedir(&dir);
    return true;
}

}  // namespace pd::fs
