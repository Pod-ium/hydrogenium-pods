#include "pd/text/text_buffer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>

namespace pd::text {

namespace {
constexpr uint32_t kIoSize = 4096;
uint8_t g_io[kIoSize];  // shared I/O scratch (single-threaded firmware)
constexpr uint32_t kNotFound = UINT32_MAX;
}  // namespace

TextBuffer::TextBuffer() {
    pages_ = new Page[kPageCount];
    tail_ = new uint8_t[kTailSize];
}

TextBuffer::~TextBuffer() {
    delete[] pages_;
    delete[] tail_;
}

bool TextBuffer::init(const std::string& workDir) {
    workDir_ = workDir;
    addPath_ = workDir + "/add.tmp";
    return newDocument();
}

// ---------------------------------------------------------------------------
// Sources and page cache
// ---------------------------------------------------------------------------

void TextBuffer::invalidatePages(uint8_t src) {
    for (int i = 0; i < kPageCount; ++i)
        if (pages_[i].src == src) pages_[i].src = 0xFF;
    lastPage_ = -1;
}

const uint8_t* TextBuffer::page(uint8_t src, uint32_t off, uint32_t* avail) {
    const uint32_t base = off & ~uint32_t(kPageSize - 1);
    int hit = -1;
    if (lastPage_ >= 0 && pages_[lastPage_].src == src && pages_[lastPage_].base == base) {
        hit = lastPage_;
    } else {
        for (int i = 0; i < kPageCount; ++i) {
            if (pages_[i].src == src && pages_[i].base == base) { hit = i; break; }
        }
    }
    if (hit < 0) {
        // Evict least recently used.
        int victim = 0;
        for (int i = 0; i < kPageCount; ++i) {
            if (pages_[i].src == 0xFF) { victim = i; break; }
            if (pages_[i].lru < pages_[victim].lru) victim = i;
        }
        Page& pg = pages_[victim];
        fs::File& f = src ? add_ : orig_;
        pg.src = 0xFF;
        if (!f.is_open() || !f.seek(base)) return nullptr;
        int r = f.read(pg.data, kPageSize);
        if (r <= 0) return nullptr;
        pg.valid = uint16_t(r);
        pg.base = base;
        pg.src = src;
        hit = victim;
    }
    Page& pg = pages_[hit];
    pg.lru = ++lruClock_;
    lastPage_ = hit;
    const uint32_t rel = off - base;
    if (rel >= pg.valid) return nullptr;
    *avail = pg.valid - rel;
    return pg.data + rel;
}

uint32_t TextBuffer::readSource(uint32_t start, uint8_t* buf, uint32_t n) {
    const uint8_t src = (start & kAddFlag) ? 1 : 0;
    const uint32_t off = start & ~kAddFlag;
    uint32_t done = 0;
    while (done < n) {
        const uint32_t o = off + done;
        if (src == 1 && o >= addFlushed_) {
            const uint32_t t = o - addFlushed_;
            if (t >= tailLen_) break;
            const uint32_t c = std::min(n - done, tailLen_ - t);
            memcpy(buf + done, tail_ + t, c);
            done += c;
            continue;
        }
        uint32_t avail = 0;
        const uint8_t* p = page(src, o, &avail);
        if (!p) break;
        uint32_t c = std::min(avail, n - done);
        if (src == 1) c = std::min(c, addFlushed_ - o);
        memcpy(buf + done, p, c);
        done += c;
    }
    return done;
}

bool TextBuffer::flushTail() {
    if (tailLen_ == 0) return true;
    if (!add_.seek(addFlushed_)) return false;
    if (add_.write(tail_, tailLen_) != int(tailLen_)) return false;
    addFlushed_ += tailLen_;
    tailLen_ = 0;
    return true;
}

bool TextBuffer::appendAdd(const uint8_t* data, uint32_t n) {
    if (uint64_t(addLen()) + n >= kAddFlag) {
        err_ = "Edit buffer full - please save";
        return false;
    }
    while (n > 0) {
        const uint32_t c = std::min(n, kTailSize - tailLen_);
        memcpy(tail_ + tailLen_, data, c);
        tailLen_ += c;
        data += c;
        n -= c;
        if (tailLen_ == kTailSize && !flushTail()) {
            err_ = "SD write error (edit buffer)";
            return false;
        }
    }
    return true;
}

bool TextBuffer::resetAdd() {
    add_.close();
    addFlushed_ = 0;
    tailLen_ = 0;
    invalidatePages(1);
    if (!add_.open(addPath_.c_str(), fs::Mode::ReadWrite)) {
        err_ = "Cannot create " + addPath_ + " (" + fs::last_error_text() + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Piece table primitives
// ---------------------------------------------------------------------------

void TextBuffer::locate(uint32_t pos, size_t* idx, uint32_t* pieceStart) {
    size_t i = cacheIdx_;
    uint32_t s = cacheStart_;
    if (i > pieces_.size()) { i = 0; s = 0; }
    while (i > 0 && pos < s) {
        --i;
        s -= pieces_[i].len;
    }
    while (i < pieces_.size() && pos >= s + pieces_[i].len) {
        s += pieces_[i].len;
        ++i;
    }
    cacheIdx_ = i;
    cacheStart_ = s;
    *idx = i;
    *pieceStart = s;
}

void TextBuffer::appendPiece(std::vector<Piece>& v, const Piece& p) {
    if (p.len == 0) return;
    if (!v.empty() && contiguous(v.back(), p)) v.back().len += p.len;
    else v.push_back(p);
}

void TextBuffer::mergeAround(size_t j) {
    if (j > 0 && j < pieces_.size() && contiguous(pieces_[j - 1], pieces_[j])) {
        pieces_[j - 1].len += pieces_[j].len;
        pieces_.erase(pieces_.begin() + j);
    }
}

void TextBuffer::insertPieces(uint32_t pos, const std::vector<Piece>& pcs) {
    if (pcs.empty()) return;
    if (pos < dirtyFrom_) dirtyFrom_ = pos;
    size_t i;
    uint32_t s;
    locate(pos, &i, &s);
    if (i < pieces_.size() && pos > s) {  // split piece i
        Piece& p = pieces_[i];
        const uint32_t off = pos - s;
        Piece right{p.start + off, p.len - off};
        p.len = off;
        pieces_.insert(pieces_.begin() + i + 1, right);
        ++i;
    }
    uint32_t total = 0;
    for (const Piece& p : pcs) total += p.len;
    pieces_.insert(pieces_.begin() + i, pcs.begin(), pcs.end());
    size_ += total;
    mergeAround(i + pcs.size());
    mergeAround(i);
    cacheIdx_ = 0;
    cacheStart_ = 0;
}

void TextBuffer::removeRange(uint32_t pos, uint32_t n, std::vector<Piece>* removed) {
    if (pos >= size_) return;
    n = std::min(n, size_ - pos);
    if (n == 0) return;
    if (pos < dirtyFrom_) dirtyFrom_ = pos;
    size_t i;
    uint32_t s;
    locate(pos, &i, &s);
    if (pos > s) {
        Piece& p = pieces_[i];
        const uint32_t off = pos - s;
        Piece right{p.start + off, p.len - off};
        p.len = off;
        pieces_.insert(pieces_.begin() + i + 1, right);
        ++i;
    }
    size_t j = i;
    uint32_t rem = n;
    while (j < pieces_.size() && pieces_[j].len <= rem) {
        if (removed) appendPiece(*removed, pieces_[j]);
        rem -= pieces_[j].len;
        ++j;
    }
    if (rem > 0 && j < pieces_.size()) {
        Piece& p = pieces_[j];
        if (removed) appendPiece(*removed, Piece{p.start, rem});
        p.start += rem;
        p.len -= rem;
        rem = 0;
    }
    pieces_.erase(pieces_.begin() + i, pieces_.begin() + j);
    size_ -= (n - rem);
    mergeAround(i);
    cacheIdx_ = 0;
    cacheStart_ = 0;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

uint8_t TextBuffer::at(uint32_t pos) {
    size_t i;
    uint32_t s;
    locate(pos, &i, &s);
    if (i >= pieces_.size()) return 0;
    uint8_t b = 0;
    readSource(pieces_[i].start + (pos - s), &b, 1);
    return b;
}

uint32_t TextBuffer::read(uint32_t pos, uint8_t* buf, uint32_t n) {
    if (pos >= size_) return 0;
    n = std::min(n, size_ - pos);
    size_t i;
    uint32_t s;
    locate(pos, &i, &s);
    uint32_t done = 0;
    while (done < n && i < pieces_.size()) {
        const Piece& p = pieces_[i];
        const uint32_t off = (pos + done) - s;
        const uint32_t c = std::min(p.len - off, n - done);
        const uint32_t r = readSource(p.start + off, buf + done, c);
        done += r;
        if (r < c) break;
        s += p.len;
        ++i;
    }
    return done;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool TextBuffer::newDocument() {
    orig_.close();
    origSize_ = 0;
    invalidatePages(0);
    path_.clear();
    pieces_.clear();
    size_ = 0;
    dirtyFrom_ = 0;
    cacheIdx_ = 0;
    cacheStart_ = 0;
    undo_.clear();
    redo_.clear();
    canMerge_ = false;
    modified_ = false;
    return resetAdd();
}

bool TextBuffer::load(const std::string& path) {
    {
        fs::File probe;  // make sure it opens before dropping the current doc
        if (!probe.open(path.c_str(), fs::Mode::Read)) {
            err_ = "Cannot open " + path + " (" + fs::last_error_text() + ")";
            return false;
        }
        if (probe.size() >= kAddFlag) {
            err_ = "File too large (max 2 GB)";
            return false;
        }
    }
    if (!newDocument()) return false;
    if (!orig_.open(path.c_str(), fs::Mode::Read)) {
        err_ = "Cannot open " + path + " (" + fs::last_error_text() + ")";
        return false;
    }
    origSize_ = orig_.size();
    if (origSize_ > 0) pieces_.push_back(Piece{0, origSize_});
    size_ = origSize_;
    path_ = path;
    return true;
}

bool TextBuffer::saveTo(const std::string& path, const Progress& progress) {
    const std::string tmp = path + ".~fw";
    {
        fs::File out;
        if (!out.open(tmp.c_str(), fs::Mode::Write)) {
            err_ = "Cannot create " + tmp;
            return false;
        }
        uint32_t done = 0;
        for (const Piece& p : pieces_) {
            uint32_t off = 0;
            while (off < p.len) {
                const uint32_t c = std::min(kIoSize, p.len - off);
                if (readSource(p.start + off, g_io, c) != c) {
                    err_ = "SD read error while saving";
                    out.close();
                    fs::remove(tmp.c_str());
                    return false;
                }
                if (out.write(g_io, c) != int(c)) {
                    err_ = "SD write error (card full?)";
                    out.close();
                    fs::remove(tmp.c_str());
                    return false;
                }
                off += c;
                done += c;
                if (progress && !progress(done, size_)) {
                    err_ = "Save cancelled";
                    out.close();
                    fs::remove(tmp.c_str());
                    return false;
                }
            }
        }
        if (!out.sync()) {
            err_ = "SD write error (sync)";
            out.close();
            fs::remove(tmp.c_str());
            return false;
        }
        out.close();
    }
    const bool same = strcasecmp(path.c_str(), path_.c_str()) == 0;
    if (same) orig_.close();  // FatFs cannot unlink an open file safely
    if (fs::exists(path.c_str()) && !fs::remove(path.c_str())) {
        err_ = "Cannot replace " + path;
        if (same) orig_.open(path_.c_str(), fs::Mode::Read);
        fs::remove(tmp.c_str());
        return false;
    }
    if (!fs::rename(tmp.c_str(), path.c_str())) {
        // The data is safe in tmp; switch to it so nothing is lost.
        load(tmp);
        modified_ = true;
        err_ = "Rename failed, text kept in " + tmp;
        return false;
    }
    // The file is safely on the card now; reopen it as the new original.
    // Retry a few times: the card can still be busy finishing the FAT and
    // directory updates of the remove/rename above.
    reopenFailed_ = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (load(path)) return true;
    }
    // Could not reopen. The old pieces point into a file that no longer
    // exists, so fall back to an empty untitled document rather than
    // keeping a buffer that would read garbage (and could be saved over the
    // good file).
    const std::string why = err_;
    newDocument();
    reopenFailed_ = true;
    err_ = "Saved, but reopening failed: " + why;
    return false;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool TextBuffer::insert(uint32_t pos, const char* s, uint32_t n) {
    if (n == 0) return true;
    if (pos > size_) pos = size_;
    if (pieces_.size() + 2 > kMaxPieces) {
        err_ = "Too many edits - save to compact";
        return false;
    }
    const uint32_t a = addLen();
    if (!appendAdd(reinterpret_cast<const uint8_t*>(s), n)) return false;
    std::vector<Piece> pcs{Piece{a | kAddFlag, n}};
    insertPieces(pos, pcs);
    pushUndo(0, pos, std::move(pcs));
    if (n == 1 && (s[0] == ' ' || s[0] == '\n' || s[0] == '\t')) canMerge_ = false;
    return true;
}

bool TextBuffer::erase(uint32_t pos, uint32_t n) {
    if (pos >= size_ || n == 0) return true;
    if (pieces_.size() + 2 > kMaxPieces) {
        err_ = "Too many edits - save to compact";
        return false;
    }
    std::vector<Piece> removed;
    removeRange(pos, n, &removed);
    pushUndo(1, pos, std::move(removed));
    return true;
}

bool TextBuffer::copyToFile(uint32_t pos, uint32_t n, const std::string& path) {
    fs::File out;
    if (!out.open(path.c_str(), fs::Mode::Write)) {
        err_ = "Cannot write clipboard";
        return false;
    }
    uint32_t off = 0;
    while (off < n) {
        const uint32_t c = read(pos + off, g_io, std::min(kIoSize, n - off));
        if (c == 0 || out.write(g_io, c) != int(c)) {
            err_ = "Clipboard write error";
            return false;
        }
        off += c;
    }
    out.close();
    return true;
}

bool TextBuffer::insertFile(uint32_t pos, const std::string& path, uint32_t* outLen) {
    *outLen = 0;
    fs::File in;
    if (!in.open(path.c_str(), fs::Mode::Read)) {
        err_ = "Clipboard is empty";
        return false;
    }
    if (pieces_.size() + 2 > kMaxPieces) {
        err_ = "Too many edits - save to compact";
        return false;
    }
    const uint32_t a = addLen();
    uint32_t total = 0;
    for (;;) {
        const int r = in.read(g_io, kIoSize);
        if (r < 0) { err_ = "Clipboard read error"; return false; }
        if (r == 0) break;
        if (!appendAdd(g_io, uint32_t(r))) return false;
        total += uint32_t(r);
    }
    if (total == 0) return true;
    if (pos > size_) pos = size_;
    std::vector<Piece> pcs{Piece{a | kAddFlag, total}};
    insertPieces(pos, pcs);
    canMerge_ = false;
    pushUndo(0, pos, std::move(pcs));
    canMerge_ = false;
    *outLen = total;
    return true;
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

size_t TextBuffer::undoPieceTotal() const {
    size_t t = 0;
    for (const UndoRec& r : undo_) t += r.pcs.size();
    for (const UndoRec& r : redo_) t += r.pcs.size();
    return t;
}

void TextBuffer::trimUndo() {
    while (undo_.size() > 1 &&
           (undo_.size() > kMaxUndoRecords || undoPieceTotal() > kMaxUndoPieces)) {
        undo_.erase(undo_.begin());
    }
}

void TextBuffer::pushUndo(uint8_t kind, uint32_t pos, std::vector<Piece>&& pcs) {
    redo_.clear();
    modified_ = true;
    uint32_t len = 0;
    for (const Piece& p : pcs) len += p.len;
    if (len == 0) return;
    if (canMerge_ && activeGroup_ == 0 && !undo_.empty()) {
        UndoRec& last = undo_.back();
        if (last.group == 0 && last.kind == kind) {
            if (kind == 0 && pos == last.pos + last.len) {
                for (const Piece& p : pcs) appendPiece(last.pcs, p);
                last.len += len;
                return;
            }
            if (kind == 1 && pos + len == last.pos) {  // backspace run
                std::vector<Piece> merged = std::move(pcs);
                for (const Piece& p : last.pcs) appendPiece(merged, p);
                last.pcs.swap(merged);
                last.pos = pos;
                last.len += len;
                return;
            }
            if (kind == 1 && pos == last.pos) {  // forward-delete run
                for (const Piece& p : pcs) appendPiece(last.pcs, p);
                last.len += len;
                return;
            }
        }
    }
    undo_.push_back(UndoRec{kind, pos, len, activeGroup_, std::move(pcs)});
    canMerge_ = true;
    trimUndo();
}

void TextBuffer::beginGroup() {
    if (groupDepth_++ == 0) activeGroup_ = groupCounter_++;
    canMerge_ = false;
}

void TextBuffer::endGroup() {
    if (groupDepth_ > 0 && --groupDepth_ == 0) activeGroup_ = 0;
    canMerge_ = false;
}

bool TextBuffer::apply(const UndoRec& r, bool inverse, uint32_t* cursor) {
    const bool doInsert = (r.kind == 0) != inverse;
    if (doInsert) {
        if (pieces_.size() + r.pcs.size() + 2 > kMaxPieces) {
            err_ = "Too many edits - save to compact";
            return false;
        }
        insertPieces(r.pos, r.pcs);
        *cursor = r.pos + r.len;
    } else {
        removeRange(r.pos, r.len, nullptr);
        *cursor = r.pos;
    }
    return true;
}

bool TextBuffer::undo(uint32_t* cursor) {
    if (undo_.empty()) return false;
    const uint32_t g = undo_.back().group;
    do {
        if (!apply(undo_.back(), true, cursor)) return false;
        redo_.push_back(std::move(undo_.back()));
        undo_.pop_back();
    } while (g != 0 && !undo_.empty() && undo_.back().group == g);
    canMerge_ = false;
    modified_ = true;
    return true;
}

bool TextBuffer::redo(uint32_t* cursor) {
    if (redo_.empty()) return false;
    const uint32_t g = redo_.back().group;
    do {
        if (!apply(redo_.back(), false, cursor)) return false;
        undo_.push_back(std::move(redo_.back()));
        redo_.pop_back();
    } while (g != 0 && !redo_.empty() && redo_.back().group == g);
    canMerge_ = false;
    modified_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

uint32_t TextBuffer::find(const std::string& needleIn, uint32_t from, bool wrap,
                          bool matchCase, const Progress& progress) {
    const uint32_t m = needleIn.size();
    if (m == 0 || m > 512 || m > size_) return kNotFound;
    std::string needle = needleIn;
    if (!matchCase)
        for (char& c : needle) c = char(tolower(uint8_t(c)));
    const uint8_t n0 = uint8_t(needle[0]);

    auto scan = [&](uint32_t a, uint32_t b) -> uint32_t {
        uint32_t p = a;
        while (p < b) {
            const uint32_t got = read(p, g_io, kIoSize);
            if (got < m) return kNotFound;
            if (!matchCase)
                for (uint32_t k = 0; k < got; ++k) g_io[k] = uint8_t(tolower(g_io[k]));
            const uint32_t last = got - m;
            for (uint32_t k = 0; k <= last; ++k) {
                if (p + k >= b) return kNotFound;
                if (g_io[k] == n0 && memcmp(g_io + k, needle.data(), m) == 0) return p + k;
            }
            p += last + 1;
            if (progress && !progress(p, size_)) return kNotFound;
        }
        return kNotFound;
    };

    if (from > size_) from = size_;
    uint32_t r = scan(from, size_);
    if (r == kNotFound && wrap && from > 0) r = scan(0, from);
    return r;
}

}  // namespace pd::text
