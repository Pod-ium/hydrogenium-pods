// PicoPods - file system.
//
// Device: FatFs (FAT32 / exFAT, UTF-8 long names) on the terminal's block
// device (the SD card). Host: plain POSIX files under a root folder. Both
// builds use this API only, so code on top of it (text buffer, ePub export,
// settings, the file browser) runs unchanged in host tests.
//
// Paths are absolute, '/' separated, UTF-8: "/books/notes.md".
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pd::hal { class BlockDevice; }

namespace pd::fs {

enum class Mode {
    Read,        // existing file, read only
    Write,       // create or truncate, write only
    ReadWrite,   // create or truncate, read and write
    Append,      // open or create, writes go to the end
    Update,      // existing file, read and write, no truncation
};

class File {
public:
    File();
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool open(const char* path, Mode mode);
    bool open(const std::string& path, Mode mode) { return open(path.c_str(), mode); }
    void close();
    bool is_open() const { return open_; }
    bool seek(uint32_t pos);
    uint32_t tell();
    // Bytes transferred, or -1 on error.
    int read(void* buf, uint32_t n);
    int write(const void* buf, uint32_t n);
    uint32_t size();
    bool sync();

private:
    bool open_ = false;
    void* impl_ = nullptr;   // FIL* on the device, FILE* on the host
};

struct DirEntry {
    std::string name;
    bool is_dir;
    uint32_t size;
};

struct Info {
    uint32_t size = 0;
    bool is_dir = false;
    uint32_t mtime = 0;   // opaque stamp: changes when the file is modified
};

// ---- volume
// Device: which block device FatFs uses (the terminal sets it). Host: ignored.
void set_block_device(hal::BlockDevice* dev);
// Host: folder that "/" maps to ("" = paths are used as they are). Device: ignored.
void set_root(const std::string& host_dir);
bool mount();
void unmount();
bool mounted();
// Capacity and free space in bytes (false if unknown).
bool space(uint64_t* total, uint64_t* free);

// ---- errors: code of the last failed call (FRESULT / errno) and a short text
int last_error();
const char* last_error_text();

// ---- paths
bool stat(const char* path, Info* info);
bool exists(const char* path);
bool is_dir(const char* path);
bool remove(const char* path);
bool rename(const char* from, const char* to);
bool mkdir(const char* path);    // true if it exists afterwards
bool mkdirs(const char* path);   // creates missing parents too
// Copies the timestamp and the read-only/hidden/system/archive attributes of
// `from` to `to` (a file copy keeps its date). False if unsupported.
bool copy_metadata(const char* from, const char* to);
// Clears the read-only attribute, so remove() and overwriting work.
bool make_writable(const char* path);
// Lists a folder (never "." or ".."). Hidden entries (dot names, and FAT
// hidden/system attributes on the device) are skipped unless include_hidden.
bool list_dir(const char* path, std::vector<DirEntry>& out, size_t max_entries,
              bool include_hidden = false);

// ---- whole-file helpers
bool read_file(const char* path, std::string& out, size_t max_bytes = 1u << 20);
// Writes via "<path>.tmp" + rename when atomic, so a crash never leaves a
// half-written file.
bool write_file(const char* path, const void* data, size_t n, bool atomic = true);
inline bool write_file(const char* path, const std::string& s, bool atomic = true) {
    return write_file(path, s.data(), s.size(), atomic);
}

}  // namespace pd::fs
