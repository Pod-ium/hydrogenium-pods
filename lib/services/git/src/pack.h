// pd_git internal: reading a Git pack file from the SD card.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include "mbedtls/sha1.h"
#include "pd/fs.h"

namespace pd::git::detail {

enum ObjType { OBJ_NONE = 0, OBJ_COMMIT = 1, OBJ_TREE = 2, OBJ_BLOB = 3, OBJ_TAG = 4, OBJ_OFS_DELTA = 6, OBJ_REF_DELTA = 7 };
const char* type_name(int t);

struct Sha {
    uint8_t b[20];
    bool zero() const;
    std::string hex() const;
    bool from_hex(const char* h);   // 40 hex characters
    bool operator==(const Sha& o) const;
    bool operator<(const Sha& o) const;
};

// Incremental SHA-1.
class Sha1 {
public:
    Sha1();
    ~Sha1();
    void update(const void* d, size_t n);
    Sha finish();

private:
    mbedtls_sha1_context ctx_;
};

// Streams one zlib stream out of a file, starting at a file offset.
class Inflater {
public:
    Inflater();
    ~Inflater();
    bool begin(fs::File& f, uint32_t offset);
    // Output bytes: n > 0, 0 at the end of the stream, -1 on corrupt data.
    int read(uint8_t* out, uint32_t n);
    // After the end: the file offset just past the stream.
    uint32_t end_offset() const { return end_offset_; }
    // CRC-32 of the compressed bytes consumed (set before begin()).
    bool want_crc = false;
    uint32_t crc = 0;

private:
    struct State;
    State* s_;
    uint32_t end_offset_ = 0;
};

// An object's content: in RAM, or in a file when large.
struct Content {
    int type = OBJ_NONE;
    uint64_t size = 0;
    std::vector<uint8_t> mem;
    std::string path;          // file holding the content (if not in mem)
    bool temp = false;         // path is a temporary file to delete
    bool in_file() const { return !path.empty(); }
    void release();            // frees RAM / deletes a temporary file
};

class Pack {
public:
    struct Entry {
        uint32_t offset;
        uint32_t crc;
    };

    // path: the .pack file; tmp_dir: where temporary files go.
    bool open(const std::string& path, const std::string& tmp_dir, std::string* err);
    void close();
    uint32_t count() const { return count_; }

    // Pass 1: reads every object; IDs of whole objects are known after it.
    // Pass 2: rebuilds delta objects to get their IDs.
    // progress(done, total) returns false to cancel.
    bool index(const std::function<bool(uint32_t, uint32_t)>& progress, std::string* err);
    bool resolve(const std::function<bool(uint32_t, uint32_t)>& progress, std::string* err);

    // Sorts by object ID and writes a version-2 .idx next to the pack.
    bool write_idx(const std::string& idx_path, const Sha& pack_sha, std::string* err);

    // Offset of an object (after write_idx sorted the table), or false.
    bool find(const Sha& id, uint32_t* offset) const;
    // Reads a whole object. target: "" = RAM if small else a temporary file;
    // otherwise the file to write the content to.
    bool load(uint32_t offset, Content& out, const std::string& target, std::string* err);

private:
    bool header_at(uint32_t off, int* type, uint64_t* size, uint32_t* data_off, uint32_t* base_off, Sha* base_id);
    bool load_at(uint32_t offset, Content& out, const std::string& target, int depth, std::string* err);
    bool hash_content(const Content& c, Sha& out, std::string* err);
    std::string temp_path();
    bool cache_get(uint32_t off, Content& out);
    void cache_put(uint32_t off, const Content& c);

    fs::File file_;
    std::string path_, tmp_dir_;
    uint32_t count_ = 0, data_end_ = 0;
    std::vector<Entry> entries_;   // pack order, later sorted with ids_
    std::vector<Sha> ids_;         // zero = not known yet
    bool sorted_ = false;
    uint32_t tmp_seq_ = 0;
    struct Cached {
        uint32_t off;
        Content c;
    };
    std::list<Cached> cache_;
    uint32_t cache_bytes_ = 0;
};

}  // namespace pd::git::detail
