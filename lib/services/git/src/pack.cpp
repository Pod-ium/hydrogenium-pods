// pd_git internal: pack reading, indexing, delta resolution, .idx writing.
#include "pack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "miniz.h"
#include "pd/git.h"

namespace pd::git {

Limits& limits() {
#if PD_HOST
    static Limits l{2000000, 4u << 20, 32u << 20};
#else
    // Worst case while rebuilding deltas: object table (28 B x max_objects)
    // + inflater (~47 KB) + base and result (2 x mem_object_bytes) + cache.
    // 5000 objects: ~140 + 47 + 64 + 32 KB, within the ~400 KB free heap.
    static Limits l{5000, 32u << 10, 32u << 10};
#endif
    return l;
}

namespace detail {

namespace {
constexpr uint32_t kIo = 4096;
constexpr int kMaxDepth = 100;
}  // namespace

const char* type_name(int t) {
    switch (t) {
        case OBJ_COMMIT: return "commit";
        case OBJ_TREE: return "tree";
        case OBJ_BLOB: return "blob";
        case OBJ_TAG: return "tag";
        default: return "?";
    }
}

// ------------------------------------------------------------------ Sha
bool Sha::zero() const {
    for (uint8_t x : b)
        if (x) return false;
    return true;
}
std::string Sha::hex() const {
    static const char d[] = "0123456789abcdef";
    std::string s(40, '0');
    for (int i = 0; i < 20; ++i) {
        s[size_t(2 * i)] = d[b[i] >> 4];
        s[size_t(2 * i + 1)] = d[b[i] & 15];
    }
    return s;
}
bool Sha::from_hex(const char* h) {
    for (int i = 0; i < 20; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = h[2 * i + k];
            int x;
            if (c >= '0' && c <= '9') x = c - '0';
            else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
            else return false;
            v = v * 16 + x;
        }
        b[i] = uint8_t(v);
    }
    return true;
}
bool Sha::operator==(const Sha& o) const { return memcmp(b, o.b, 20) == 0; }
bool Sha::operator<(const Sha& o) const { return memcmp(b, o.b, 20) < 0; }

Sha1::Sha1() {
    mbedtls_sha1_init(&ctx_);
    mbedtls_sha1_starts_ret(&ctx_);
}
Sha1::~Sha1() { mbedtls_sha1_free(&ctx_); }
void Sha1::update(const void* d, size_t n) { mbedtls_sha1_update_ret(&ctx_, static_cast<const unsigned char*>(d), n); }
Sha Sha1::finish() {
    Sha s;
    mbedtls_sha1_finish_ret(&ctx_, s.b);
    return s;
}

// ------------------------------------------------------------------ Inflater
struct Inflater::State {
    tinfl_decompressor d;
    uint8_t dict[TINFL_LZ_DICT_SIZE];
    uint8_t in[kIo];
    size_t in_pos = 0, in_len = 0;
    fs::File* f = nullptr;
    uint32_t next_read = 0;
    size_t dict_ofs = 0, avail_pos = 0, avail_len = 0;
    bool done = false, eof = false;
};

Inflater::Inflater() : s_(new State()) {}
Inflater::~Inflater() { delete s_; }

bool Inflater::begin(fs::File& f, uint32_t offset) {
    tinfl_init(&s_->d);
    s_->f = &f;
    s_->next_read = offset;
    s_->in_pos = s_->in_len = 0;
    s_->dict_ofs = s_->avail_pos = s_->avail_len = 0;
    s_->done = s_->eof = false;
    end_offset_ = 0;
    return true;
}

int Inflater::read(uint8_t* out, uint32_t n) {
    State& s = *s_;
    uint32_t produced = 0;
    while (produced < n) {
        if (s.avail_len) {
            const size_t k = std::min<size_t>(s.avail_len, n - produced);
            memcpy(out + produced, s.dict + s.avail_pos, k);
            s.avail_pos += k;
            s.avail_len -= k;
            produced += uint32_t(k);
            continue;
        }
        if (s.done) break;
        if (s.in_pos == s.in_len && !s.eof) {
            if (!s.f->seek(s.next_read)) return -1;
            const int r = s.f->read(s.in, kIo);
            if (r < 0) return -1;
            if (r == 0) s.eof = true;
            s.in_pos = 0;
            s.in_len = size_t(r);
            s.next_read += uint32_t(r);
        }
        size_t in_bytes = s.in_len - s.in_pos;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - s.dict_ofs;
        const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (s.eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        const tinfl_status st =
            tinfl_decompress(&s.d, s.in + s.in_pos, &in_bytes, s.dict, s.dict + s.dict_ofs, &out_bytes, flags);
        if (want_crc && in_bytes) crc = uint32_t(mz_crc32(crc, s.in + s.in_pos, in_bytes));
        s.in_pos += in_bytes;
        s.avail_pos = s.dict_ofs;
        s.avail_len = out_bytes;
        s.dict_ofs = (s.dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) {
            s.done = true;
            end_offset_ = s.next_read - uint32_t(s.in_len - s.in_pos);
        } else if (st < 0) {
            return -1;
        } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && s.eof && s.in_pos == s.in_len) {
            return -1;   // stream cut short
        }
    }
    return int(produced);
}

// ------------------------------------------------------------------ helpers
void Content::release() {
    std::vector<uint8_t>().swap(mem);
    if (temp && !path.empty()) fs::remove(path.c_str());
    path.clear();
    temp = false;
}

namespace {

// Buffered output to RAM or a file.
class Sink {
public:
    bool open_file(const std::string& p) {
        buf_.resize(kIo);
        return f_.open(p, fs::Mode::Write);
    }
    void to_mem(std::vector<uint8_t>* m, uint64_t reserve) {
        mem_ = m;
        mem_->clear();
        mem_->reserve(size_t(reserve));
    }
    bool write(const uint8_t* d, size_t n) {
        if (mem_) {
            mem_->insert(mem_->end(), d, d + n);
            return true;
        }
        while (n) {
            const size_t k = std::min(n, buf_.size() - used_);
            memcpy(&buf_[used_], d, k);
            used_ += k;
            d += k;
            n -= k;
            if (used_ == buf_.size() && !flush()) return false;
        }
        return true;
    }
    bool finish() {
        if (mem_) return true;
        const bool ok = flush();
        f_.close();
        return ok;
    }

private:
    bool flush() {
        if (!used_) return true;
        const bool ok = f_.write(buf_.data(), uint32_t(used_)) == int(used_);
        used_ = 0;
        return ok;
    }
    std::vector<uint8_t>* mem_ = nullptr;
    fs::File f_;
    std::vector<uint8_t> buf_;
    size_t used_ = 0;
};

// Byte-level reader over an Inflater (delta instructions).
class ByteReader {
public:
    explicit ByteReader(Inflater& in) : in_(in) {}
    int get() {
        if (pos_ == len_) {
            const int r = in_.read(buf_, sizeof buf_);
            if (r <= 0) return -1;
            pos_ = 0;
            len_ = size_t(r);
        }
        return buf_[pos_++];
    }
    bool read(uint8_t* d, size_t n) {
        while (n) {
            if (pos_ == len_) {
                const int r = in_.read(buf_, sizeof buf_);
                if (r <= 0) return false;
                pos_ = 0;
                len_ = size_t(r);
            }
            const size_t k = std::min(n, len_ - pos_);
            memcpy(d, buf_ + pos_, k);
            pos_ += k;
            d += k;
            n -= k;
        }
        return true;
    }
    bool varint(uint64_t* v) {
        uint64_t x = 0;
        int shift = 0, c;
        do {
            if ((c = get()) < 0 || shift > 56) return false;
            x |= uint64_t(c & 0x7F) << shift;
            shift += 7;
        } while (c & 0x80);
        *v = x;
        return true;
    }

private:
    Inflater& in_;
    uint8_t buf_[1024];
    size_t pos_ = 0, len_ = 0;
};

void put_be32(std::string& s, uint32_t v) {
    s += char(v >> 24);
    s += char(v >> 16);
    s += char(v >> 8);
    s += char(v);
}

}  // namespace

// ------------------------------------------------------------------ Pack
bool Pack::open(const std::string& path, const std::string& tmp_dir, std::string* err) {
    path_ = path;
    tmp_dir_ = tmp_dir;
    if (!file_.open(path, fs::Mode::Read)) {
        if (err) *err = "cannot open the pack";
        return false;
    }
    uint8_t h[12];
    if (file_.read(h, 12) != 12 || memcmp(h, "PACK", 4) != 0) {
        if (err) *err = "not a pack file";
        return false;
    }
    const uint32_t ver = (uint32_t(h[4]) << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
    count_ = (uint32_t(h[8]) << 24) | (h[9] << 16) | (h[10] << 8) | h[11];
    if (ver != 2 && ver != 3) {
        if (err) *err = "unsupported pack version";
        return false;
    }
    const uint32_t sz = file_.size();
    if (sz < 32) {
        if (err) *err = "pack too short";
        return false;
    }
    data_end_ = sz - 20;
    if (count_ > limits().max_objects) {
        if (err)
            *err = "repository too large for this device (" + std::to_string(count_) + " objects, limit " +
                   std::to_string(limits().max_objects) + ")";
        return false;
    }
    entries_.assign(count_, Entry{0, 0});
    ids_.assign(count_, Sha{});
    return true;
}

void Pack::close() {
    file_.close();
    for (Cached& c : cache_) c.c.release();
    cache_.clear();
    cache_bytes_ = 0;
    std::vector<Entry>().swap(entries_);
    std::vector<Sha>().swap(ids_);
}

std::string Pack::temp_path() { return tmp_dir_ + "/t" + std::to_string(++tmp_seq_); }

bool Pack::header_at(uint32_t off, int* type, uint64_t* size, uint32_t* data_off, uint32_t* base_off, Sha* base_id) {
    uint8_t b[40];
    if (!file_.seek(off)) return false;
    const int n = file_.read(b, sizeof b);
    if (n < 2) return false;
    int i = 0;
    uint8_t c = b[i++];
    *type = (c >> 4) & 7;
    uint64_t sz = c & 15;
    int shift = 4;
    while (c & 0x80) {
        if (i >= n || shift > 57) return false;
        c = b[i++];
        sz |= uint64_t(c & 0x7F) << shift;
        shift += 7;
    }
    *size = sz;
    if (*type == OBJ_OFS_DELTA) {
        if (i >= n) return false;
        c = b[i++];
        uint64_t ofs = c & 0x7F;
        while (c & 0x80) {
            if (i >= n) return false;
            c = b[i++];
            ofs = ((ofs + 1) << 7) | (c & 0x7F);
        }
        if (ofs == 0 || ofs > off) return false;
        *base_off = uint32_t(off - ofs);
    } else if (*type == OBJ_REF_DELTA) {
        if (i + 20 > n) return false;
        memcpy(base_id->b, b + i, 20);
        i += 20;
    } else if (*type < OBJ_COMMIT || *type > OBJ_TAG) {
        return false;
    }
    *data_off = off + uint32_t(i);
    return true;
}

bool Pack::index(const std::function<bool(uint32_t, uint32_t)>& progress, std::string* err) {
    Inflater inf;
    std::vector<uint8_t> buf(kIo);
    uint32_t off = 12;
    for (uint32_t i = 0; i < count_; ++i) {
        int type;
        uint64_t size;
        uint32_t data_off, base_off = 0;
        Sha base_id;
        if (off >= data_end_ || !header_at(off, &type, &size, &data_off, &base_off, &base_id)) {
            if (err) *err = "corrupt pack (object " + std::to_string(i) + ")";
            return false;
        }
        // CRC of the header bytes, continued over the compressed data.
        uint8_t hb[40];
        file_.seek(off);
        file_.read(hb, data_off - off);
        inf.want_crc = true;
        inf.crc = uint32_t(mz_crc32(MZ_CRC32_INIT, hb, data_off - off));
        inf.begin(file_, data_off);
        const bool whole = type >= OBJ_COMMIT && type <= OBJ_TAG;
        Sha1 h;
        if (whole) {
            char head[32];
            const int hn = snprintf(head, sizeof head, "%s %llu", type_name(type), (unsigned long long)size);
            h.update(head, size_t(hn) + 1);   // with the NUL
        }
        uint64_t total = 0;
        for (;;) {
            const int r = inf.read(buf.data(), kIo);
            if (r < 0) {
                if (err) *err = "corrupt data in the pack (object " + std::to_string(i) + ")";
                return false;
            }
            if (r == 0) break;
            if (whole) h.update(buf.data(), size_t(r));
            total += uint32_t(r);
        }
        if (total != size) {
            if (err) *err = "object size mismatch in the pack";
            return false;
        }
        entries_[i] = Entry{off, inf.crc};
        if (whole) ids_[i] = h.finish();
        off = inf.end_offset();
        if (progress && (i % 16 == 0 || i + 1 == count_) && !progress(i + 1, count_)) {
            if (err) *err = "cancelled";
            return false;
        }
    }
    if (off != data_end_) {
        if (err) *err = "pack has unexpected extra data";
        return false;
    }
    return true;
}

bool Pack::hash_content(const Content& c, Sha& out, std::string* err) {
    Sha1 h;
    char head[32];
    const int hn = snprintf(head, sizeof head, "%s %llu", type_name(c.type), (unsigned long long)c.size);
    h.update(head, size_t(hn) + 1);
    if (!c.in_file()) {
        h.update(c.mem.data(), c.mem.size());
    } else {
        fs::File f;
        if (!f.open(c.path, fs::Mode::Read)) {
            if (err) *err = "cannot read a temporary file";
            return false;
        }
        std::vector<uint8_t> b(kIo);
        int r;
        while ((r = f.read(b.data(), kIo)) > 0) h.update(b.data(), size_t(r));
        if (r < 0) return false;
    }
    out = h.finish();
    return true;
}

bool Pack::cache_get(uint32_t off, Content& out) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->off != off) continue;
        out.type = it->c.type;
        out.size = it->c.size;
        out.mem = it->c.mem;
        out.path.clear();
        out.temp = false;
        cache_.splice(cache_.begin(), cache_, it);   // most recently used first
        return true;
    }
    return false;
}

void Pack::cache_put(uint32_t off, const Content& c) {
    const uint32_t budget = limits().cache_bytes;
    if (c.in_file() || c.mem.size() > budget / 4) return;
    for (const Cached& x : cache_)
        if (x.off == off) return;
    while (!cache_.empty() && cache_bytes_ + c.mem.size() > budget) {
        cache_bytes_ -= uint32_t(cache_.back().c.mem.size());
        cache_.pop_back();
    }
    Cached e;
    e.off = off;
    e.c.type = c.type;
    e.c.size = c.size;
    e.c.mem = c.mem;
    cache_bytes_ += uint32_t(c.mem.size());
    cache_.push_front(std::move(e));
}

bool Pack::load(uint32_t offset, Content& out, const std::string& target, std::string* err) {
    return load_at(offset, out, target, 0, err);
}

bool Pack::load_at(uint32_t off, Content& out, const std::string& target, int depth, std::string* err) {
    if (depth > kMaxDepth) {
        if (err) *err = "delta chain too long";
        return false;
    }
    int type;
    uint64_t size;
    uint32_t data_off, base_off = 0;
    Sha base_id;
    if (!header_at(off, &type, &size, &data_off, &base_off, &base_id)) {
        if (err) *err = "corrupt pack";
        return false;
    }
    const uint32_t mem_limit = limits().mem_object_bytes;
    if (target.empty() && cache_get(off, out)) return true;

    out.release();
    Inflater inf;
    inf.begin(file_, data_off);
    Sink sink;

    if (type != OBJ_OFS_DELTA && type != OBJ_REF_DELTA) {
        out.type = type;
        out.size = size;
        const bool to_file = !target.empty() || size > mem_limit;
        if (to_file) {
            out.path = target.empty() ? temp_path() : target;
            out.temp = target.empty();
            if (!sink.open_file(out.path)) {
                if (err) *err = "cannot create " + out.path;
                return false;
            }
        } else {
            sink.to_mem(&out.mem, size);
        }
        std::vector<uint8_t> buf(kIo);
        uint64_t total = 0;
        int r;
        while ((r = inf.read(buf.data(), kIo)) > 0) {
            if (!sink.write(buf.data(), size_t(r))) {
                if (err) *err = "write failed (card full?)";
                sink.finish();
                return false;
            }
            total += uint32_t(r);
        }
        if (!sink.finish() || r < 0 || total != size) {
            if (err) *err = r < 0 ? "corrupt data in the pack" : "write failed (card full?)";
            return false;
        }
        if (!to_file) cache_put(off, out);
        return true;
    }

    // Delta: rebuild the base first.
    if (type == OBJ_REF_DELTA) {
        bool found = false;
        for (uint32_t i = 0; i < count_; ++i)
            if (ids_[i] == base_id) {
                base_off = entries_[i].offset;
                found = true;
                break;
            }
        if (!found) {
            if (err) *err = "missing delta base " + base_id.hex();
            return false;
        }
    }
    Content base;
    if (!load_at(base_off, base, "", depth + 1, err)) return false;
    inf.begin(file_, data_off);   // the base load moved the file position; restart cleanly
    ByteReader rd(inf);
    uint64_t src_size, dst_size;
    if (!rd.varint(&src_size) || !rd.varint(&dst_size) || src_size != base.size) {
        base.release();
        if (err) *err = "corrupt delta";
        return false;
    }
    out.type = base.type;
    out.size = dst_size;
    const bool to_file = !target.empty() || dst_size > mem_limit;
    if (to_file) {
        out.path = target.empty() ? temp_path() : target;
        out.temp = target.empty();
        if (!sink.open_file(out.path)) {
            base.release();
            if (err) *err = "cannot create " + out.path;
            return false;
        }
    } else {
        sink.to_mem(&out.mem, dst_size);
    }
    fs::File bf;
    if (base.in_file() && !bf.open(base.path, fs::Mode::Read)) {
        base.release();
        sink.finish();
        if (err) *err = "cannot read a temporary file";
        return false;
    }
    std::vector<uint8_t> buf(kIo);
    uint64_t produced = 0;
    bool ok = true;
    const char* why = "corrupt delta";
    while (ok && produced < dst_size) {
        const int cmd = rd.get();
        if (cmd < 0) {
            ok = false;
        } else if (cmd & 0x80) {
            uint64_t cp_off = 0, cp_size = 0;
            for (int k = 0; k < 4 && ok; ++k)
                if (cmd & (1 << k)) {
                    const int v = rd.get();
                    ok = v >= 0;
                    cp_off |= uint64_t(v & 0xFF) << (8 * k);
                }
            for (int k = 0; k < 3 && ok; ++k)
                if (cmd & (0x10 << k)) {
                    const int v = rd.get();
                    ok = v >= 0;
                    cp_size |= uint64_t(v & 0xFF) << (8 * k);
                }
            if (cp_size == 0) cp_size = 0x10000;
            if (!ok || cp_off + cp_size > base.size || produced + cp_size > dst_size) {
                ok = false;
                break;
            }
            if (!base.in_file()) {
                ok = sink.write(base.mem.data() + cp_off, size_t(cp_size));
                if (!ok) why = "write failed (card full?)";
            } else {
                ok = bf.seek(uint32_t(cp_off));
                uint64_t left = cp_size;
                while (ok && left) {
                    const uint32_t k = uint32_t(std::min<uint64_t>(left, kIo));
                    ok = bf.read(buf.data(), k) == int(k) && sink.write(buf.data(), k);
                    left -= k;
                }
            }
            produced += cp_size;
        } else if (cmd) {
            if (produced + uint32_t(cmd) > dst_size || !rd.read(buf.data(), size_t(cmd)) ||
                !sink.write(buf.data(), size_t(cmd))) {
                ok = false;
                break;
            }
            produced += uint32_t(cmd);
        } else {
            ok = false;   // reserved instruction 0
        }
    }
    bf.close();
    base.release();
    if (!sink.finish()) {
        ok = false;
        why = "write failed (card full?)";
    }
    if (!ok) {
        if (err) *err = why;
        out.release();
        return false;
    }
    if (!to_file) cache_put(off, out);
    return true;
}

bool Pack::resolve(const std::function<bool(uint32_t, uint32_t)>& progress, std::string* err) {
    uint32_t pending = 0;
    for (const Sha& s : ids_) pending += s.zero() ? 1 : 0;
    const uint32_t total = pending;
    uint32_t done = 0;
    // REF deltas may name bases rebuilt later: repeat until nothing changes.
    while (pending) {
        uint32_t progressed = 0;
        std::string last;
        for (uint32_t i = 0; i < count_; ++i) {
            if (!ids_[i].zero()) continue;
            Content c;
            std::string e;
            if (!load_at(entries_[i].offset, c, "", 0, &e)) {
                if (e.compare(0, 19, "missing delta base ") == 0) {
                    last = e;
                    continue;
                }
                if (err) *err = e;
                return false;
            }
            Sha id;
            const bool ok = hash_content(c, id, err);
            c.release();
            if (!ok) return false;
            ids_[i] = id;
            ++progressed;
            --pending;
            ++done;
            if (progress && (done % 8 == 0 || !pending) && !progress(done, total)) {
                if (err) *err = "cancelled";
                return false;
            }
        }
        if (!progressed) {
            if (err) *err = last.empty() ? "unresolvable deltas" : last;
            return false;
        }
    }
    return true;
}

bool Pack::write_idx(const std::string& idx_path, const Sha& pack_sha, std::string* err) {
    // Sort (entries_, ids_) by id: a permutation, applied in place.
    std::vector<uint32_t> perm(count_);
    for (uint32_t i = 0; i < count_; ++i) perm[i] = i;
    std::sort(perm.begin(), perm.end(), [this](uint32_t a, uint32_t b) { return ids_[a] < ids_[b]; });
    for (uint32_t i = 0; i < count_; ++i) {
        if (perm[i] == i || perm[i] == UINT32_MAX) continue;
        // follow the cycle starting at i
        Entry e = entries_[i];
        Sha s = ids_[i];
        uint32_t j = i;
        for (;;) {
            const uint32_t k = perm[j];
            perm[j] = UINT32_MAX;
            if (k == i) {
                entries_[j] = e;
                ids_[j] = s;
                break;
            }
            entries_[j] = entries_[k];
            ids_[j] = ids_[k];
            j = k;
        }
    }
    sorted_ = true;

    fs::File f;
    if (!f.open(idx_path, fs::Mode::Write)) {
        if (err) *err = "cannot create the pack index";
        return false;
    }
    Sha1 h;
    std::string out;
    auto flush = [&](bool force) {
        if (out.size() >= kIo || (force && !out.empty())) {
            h.update(out.data(), out.size());
            const bool ok = f.write(out.data(), uint32_t(out.size())) == int(out.size());
            out.clear();
            return ok;
        }
        return true;
    };
    bool ok = true;
    out.append("\377tOc", 4);
    put_be32(out, 2);
    uint32_t n = 0;
    for (int b = 0; b < 256; ++b) {
        while (n < count_ && ids_[n].b[0] == b) ++n;
        put_be32(out, n);
    }
    for (uint32_t i = 0; i < count_ && ok; ++i) {
        out.append(reinterpret_cast<const char*>(ids_[i].b), 20);
        ok = flush(false);
    }
    for (uint32_t i = 0; i < count_ && ok; ++i) {
        put_be32(out, entries_[i].crc);
        ok = flush(false);
    }
    for (uint32_t i = 0; i < count_ && ok; ++i) {
        put_be32(out, entries_[i].offset);
        ok = flush(false);
    }
    out.append(reinterpret_cast<const char*>(pack_sha.b), 20);
    ok = ok && flush(true);
    const Sha idx_sha = h.finish();
    ok = ok && f.write(idx_sha.b, 20) == 20;
    f.close();
    if (!ok && err) *err = "cannot write the pack index (card full?)";
    return ok;
}

bool Pack::find(const Sha& id, uint32_t* offset) const {
    if (!sorted_) return false;
    const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (it == ids_.end() || !(*it == id)) return false;
    *offset = entries_[size_t(it - ids_.begin())].offset;
    return true;
}

}  // namespace detail
}  // namespace pd::git
