#include "pd/epub.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pd/fs.h"

namespace pd::epub {
namespace {

constexpr uint32_t kChapterMax = 48 * 1024;  // split chapters around this size
constexpr uint32_t kLineMax = 8 * 1024;      // longer lines are processed in pieces
constexpr size_t kParaMax = 12 * 1024;       // Markdown paragraph buffer limit
constexpr int kMaxDepth = 12;                // inline nesting limit
constexpr size_t kMaxToc = 1000;
// Fixed zip timestamp (the PicoCalc has no clock): 2026-01-01 00:00.
constexpr uint16_t kDosDate = uint16_t(((2026 - 1980) << 9) | (1 << 5) | 1);
constexpr uint16_t kDosTime = 0;

// ---------------------------------------------------------------------------
// CRC-32 and a minimal "stored" zip writer
// ---------------------------------------------------------------------------

uint32_t g_crcTable[256];

void initCrc() {
    if (g_crcTable[1]) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[i] = c;
    }
}

inline void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) {
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

class Zip {
public:
    bool open(const std::string& path) {
        initCrc();
        ok_ = f_.open(path.c_str(), fs::Mode::Write);
        if (!ok_) err_ = std::string("Cannot create file (") + fs::last_error_text() + ")";
        return ok_;
    }
    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }

    bool begin(const std::string& name) {
        if (!ok_) return false;
        flush();
        entries_.push_back(Entry{name, 0, 0, pos_});
        crc_ = 0xFFFFFFFFu;
        size_ = 0;
        uint8_t h[30];
        put32(h, 0x04034b50);
        put16(h + 4, 10);  // version needed: stored
        put16(h + 6, 0);   // flags
        put16(h + 8, 0);   // method: stored
        put16(h + 10, kDosTime);
        put16(h + 12, kDosDate);
        memset(h + 14, 0, 12);  // crc and sizes, patched in end()
        put16(h + 26, uint32_t(name.size()));
        put16(h + 28, 0);
        raw(h, sizeof h);
        raw(name.data(), uint32_t(name.size()));
        return ok_;
    }
    void write(const void* d, uint32_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        uint32_t c = crc_;
        for (uint32_t i = 0; i < n; ++i) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        crc_ = c;
        size_ += n;
        raw(p, n);
    }
    void write(const std::string& s) { write(s.data(), uint32_t(s.size())); }
    void write(const char* s) { write(s, uint32_t(strlen(s))); }
    uint32_t entrySize() const { return size_; }

    bool end() {
        if (!ok_ || entries_.empty()) return false;
        flush();
        Entry& e = entries_.back();
        e.crc = crc_ ^ 0xFFFFFFFFu;
        e.size = size_;
        uint8_t p[12];
        put32(p, e.crc);
        put32(p + 4, e.size);
        put32(p + 8, e.size);
        if (!f_.seek(e.offset + 14) || f_.write(p, 12) != 12 || !f_.seek(pos_)) fail();
        return ok_;
    }

    bool finish() {
        if (!ok_) return false;
        flush();
        const uint32_t cdStart = pos_;
        for (const Entry& e : entries_) {
            uint8_t h[46];
            put32(h, 0x02014b50);
            put16(h + 4, 20);
            put16(h + 6, 10);
            put16(h + 8, 0);
            put16(h + 10, 0);
            put16(h + 12, kDosTime);
            put16(h + 14, kDosDate);
            put32(h + 16, e.crc);
            put32(h + 20, e.size);
            put32(h + 24, e.size);
            put16(h + 28, uint32_t(e.name.size()));
            memset(h + 30, 0, 12);  // extra, comment, disk, attributes
            put32(h + 42, e.offset);
            raw(h, sizeof h);
            raw(e.name.data(), uint32_t(e.name.size()));
        }
        uint8_t z[22];
        put32(z, 0x06054b50);
        put16(z + 4, 0);
        put16(z + 6, 0);
        put16(z + 8, uint32_t(entries_.size()));
        put16(z + 10, uint32_t(entries_.size()));
        put32(z + 12, pos_ - cdStart);  // pos_ counts buffered bytes too
        put32(z + 16, cdStart);
        put16(z + 20, 0);
        raw(z, sizeof z);
        flush();
        if (ok_ && !f_.sync()) fail();
        f_.close();
        return ok_;
    }
    void close() { f_.close(); }

private:
    struct Entry {
        std::string name;
        uint32_t crc, size, offset;
    };
    void fail() {
        if (ok_) err_ = std::string("Write error (") + fs::last_error_text() + ")";
        ok_ = false;
    }
    void raw(const void* d, uint32_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        pos_ += n;
        while (n && ok_) {
            const uint32_t k = std::min<uint32_t>(n, kBufSize - bl_);
            memcpy(buf_.get() + bl_, p, k);
            bl_ += k;
            p += k;
            n -= k;
            if (bl_ == kBufSize) flush();
        }
    }
    void flush() {
        if (!bl_ || !ok_) { bl_ = 0; return; }
        if (f_.write(buf_.get(), bl_) != int(bl_)) fail();
        bl_ = 0;
    }

    fs::File f_;
    std::vector<Entry> entries_;
    uint32_t pos_ = 0, crc_ = 0, size_ = 0, bl_ = 0;
    // On the heap, not inside the object: Zip and LineReader live on the stack
    // in write()/guessTitle(), and 2 KB arrays there made write()'s frame 5 KB.
    static constexpr uint32_t kBufSize = 2048;
    std::unique_ptr<uint8_t[]> buf_{new uint8_t[kBufSize]};
    bool ok_ = false;
    std::string err_;
};

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

inline bool isSpace(uint8_t c) { return c == ' ' || c == '\t'; }
inline bool isAlnum(uint8_t c) {
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c >= 0x80;
}
inline bool isPunct(uint8_t c) { return c > 0x20 && c < 0x7F && !isAlnum(c); }

// Appends s[0, n) to out as XML character data: escapes markup characters,
// drops control characters and turns bytes that are not valid UTF-8 into
// their Latin-1 characters (the editor displays such bytes the same way).
// code: keep indentation and runs of spaces with no-break spaces.
void escapeTo(std::string& out, const char* s, size_t n, bool attr = false, bool code = false) {
    for (size_t i = 0; i < n;) {
        const uint8_t c = uint8_t(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"':
                    if (attr) out += "&quot;";
                    else out += '"';
                    break;
                case '\t':
                    if (code) out += "&#160;&#160;&#160;&#160;";
                    else out += ' ';
                    break;
                case ' ':
                    if (code && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' ||
                                 (i + 1 < n && (s[i + 1] == ' ' || s[i + 1] == '\t'))))
                        out += "&#160;";
                    else out += ' ';
                    break;
                default:
                    if (c >= 0x20 && c != 0x7F) out += char(c);
                    break;
            }
            ++i;
            continue;
        }
        // UTF-8 sequence?
        int len = 0;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        }
        bool valid = len > 0 && i + size_t(len) <= n;
        if (valid) {
            const uint8_t c1 = uint8_t(s[i + 1]);
            valid = c1 >= lo && c1 <= hi;
            for (int k = 2; valid && k < len; ++k) valid = (uint8_t(s[i + k]) & 0xC0) == 0x80;
        }
        if (valid) {
            // U+FFFE / U+FFFF are not allowed in XML
            const bool bad = len == 3 && c == 0xEF && uint8_t(s[i + 1]) == 0xBF &&
                             (uint8_t(s[i + 2]) == 0xBE || uint8_t(s[i + 2]) == 0xBF);
            if (!bad) out.append(s + i, size_t(len));
            i += size_t(len);
        } else {
            if (c >= 0xA0) {  // Latin-1 fallback
                out += char(0xC0 | (c >> 6));
                out += char(0x80 | (c & 0x3F));
            } else {
                out += '?';
            }
            ++i;
        }
    }
}

std::string esc(const std::string& s, bool attr = false) {
    std::string o;
    escapeTo(o, s.data(), s.size(), attr);
    return o;
}

// Removes tags from generated XHTML (used for TOC labels); entities stay.
std::string stripTags(const std::string& h) {
    std::string o;
    bool tag = false;
    for (char c : h) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) o += c;
    }
    size_t a = 0, b = o.size();
    while (a < b && o[a] == ' ') ++a;
    while (b > a && o[b - 1] == ' ') --b;
    return o.substr(a, b - a);
}

uint64_t fnv(uint64_t h, const void* d, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(d);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---------------------------------------------------------------------------
// Line reader over the document
// ---------------------------------------------------------------------------

class LineReader {
public:
    LineReader(const Reader& rd, uint32_t size) : rd_(rd), size_(size) {}
    uint32_t position() const { return pos_ - (blen_ - bpos_); }

    // Next line (without the line break) or piece of a line longer than
    // kLineMax (*more = true: the line continues). False at the end.
    bool next(std::string& line, bool* more) {
        line.clear();
        *more = false;
        bool any = false;
        for (;;) {
            if (bpos_ == blen_) {
                if (pos_ >= size_) break;
                blen_ = rd_(pos_, buf_.get(), std::min<uint32_t>(kBufSize, size_ - pos_));
                bpos_ = 0;
                if (blen_ == 0) { pos_ = size_; break; }
                pos_ += blen_;
            }
            any = true;
            const uint8_t* start = buf_.get() + bpos_;
            const void* nl = memchr(start, '\n', blen_ - bpos_);
            const uint32_t avail = nl ? uint32_t(static_cast<const uint8_t*>(nl) - start)
                                      : blen_ - bpos_;
            uint32_t take = avail;
            if (line.size() + take > kLineMax) {
                take = kLineMax - uint32_t(line.size());
                // cut at a UTF-8 character boundary
                while (take > 0 && (start[take] & 0xC0) == 0x80) --take;
                line.append(reinterpret_cast<const char*>(start), take);
                bpos_ += take;
                *more = true;
                return true;
            }
            line.append(reinterpret_cast<const char*>(start), take);
            bpos_ += take;
            if (nl) {
                ++bpos_;
                break;
            }
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return any;
    }

private:
    const Reader& rd_;
    uint32_t size_, pos_ = 0, bpos_ = 0, blen_ = 0;
    static constexpr uint32_t kBufSize = 2048;
    std::unique_ptr<uint8_t[]> buf_{new uint8_t[kBufSize]};  // heap, see Zip
};

// ---------------------------------------------------------------------------
// Book: chapters, TOC and the package files
// ---------------------------------------------------------------------------

struct TocEntry {
    int chapter;
    int level;
    int id;
    std::string label;  // already escaped
};

class Book {
public:
    Book(Zip& z, const Options& o) : z_(z), o_(o) {}

    void out(const char* s) { open(); z_.write(s); }
    void out(const std::string& s) { open(); z_.write(s); }
    void block() { ++blocks_; }
    // Starts a new chapter at the next output if the current one is big.
    void maybeSplit() {
        if (open_ && z_.entrySize() > kChapterMax) close();
    }
    void newChapter() {
        if (open_ && blocks_ > 0) close();
    }
    int newId() { return ++ids_; }
    void addToc(int level, int id, const std::string& label) {
        if (toc_.size() < kMaxToc) toc_.push_back(TocEntry{chapters_, level, id, label});
    }
    int chapter() const { return chapters_; }

    bool finish(uint64_t h1, uint64_t h2) {
        if (!open_ && chapters_ == 0) open();
        if (open_) close();
        char uid[64];
        snprintf(uid, sizeof uid, "urn:uuid:%08x-%04x-4%03x-%04x-%012llx", unsigned(h1 >> 32),
                 unsigned(h1 >> 16) & 0xFFFF, unsigned(h1) & 0xFFF,
                 0x8000 | (unsigned(h2 >> 48) & 0x3FFF), (unsigned long long)(h2 & 0xFFFFFFFFFFFFull));
        writeCss();
        writeNcx(uid);
        writeOpf(uid);
        return z_.ok();
    }

private:
    static std::string chapterName(int n) {
        char b[24];
        snprintf(b, sizeof b, "c%03d.xhtml", n);
        return b;
    }
    void open() {
        if (open_) return;
        ++chapters_;
        z_.begin("OEBPS/" + chapterName(chapters_));
        open_ = true;
        blocks_ = 0;
        std::string h =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
            "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" +
            esc(o_.language, true) + "\">\n<head>\n<title>" + esc(o_.title) +
            "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
            "</head>\n<body>\n";
        z_.write(h);
    }
    void close() {
        if (!open_) return;
        if (blocks_ == 0) z_.write("<p>&#160;</p>\n");
        z_.write("</body>\n</html>\n");
        z_.end();
        open_ = false;
    }
    void writeCss() {
        z_.begin("OEBPS/style.css");
        z_.write(
            "body { margin: 0; }\n"
            "h1, h2, h3, h4, h5, h6 { font-weight: bold; margin: 1em 0 0.5em 0; }\n"
            "h1 { font-size: 1.5em; }\nh2 { font-size: 1.3em; }\nh3 { font-size: 1.15em; }\n"
            "blockquote { margin: 0.5em 1.5em; font-style: italic; }\n"
            "code, .code { font-family: monospace; }\n"
            ".code { margin: 0; font-size: 0.85em; text-indent: 0; text-align: left; }\n"
            "table { border-collapse: collapse; margin: 0.5em 0; }\n"
            "th, td { border: 1px solid #888; padding: 0.2em 0.4em; }\n");
        if (o_.kind == Kind::Text)
            z_.write("p { margin: 0; text-indent: 1.2em; }\n");
        else
            z_.write("p { margin: 0 0 0.6em 0; }\n");
        z_.end();
    }
    void writeNcx(const char* uid) {
        z_.begin("OEBPS/toc.ncx");
        std::string s =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<!DOCTYPE ncx PUBLIC \"-//NISO//DTD ncx 2005-1//EN\" "
            "\"http://www.daisy.org/z3986/2005/ncx-2005-1.dtd\">\n"
            "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head>\n"
            "<meta name=\"dtb:uid\" content=\"" + std::string(uid) + "\"/>\n";
        std::vector<TocEntry> toc = toc_;
        if (toc.empty()) toc.push_back(TocEntry{1, 1, 0, esc(o_.title)});
        // Normalise levels so the first entry is the top and no level is skipped.
        int depth = 1;
        std::vector<int> stack;  // original levels of the open navPoints
        std::string body;
        int order = 0;
        for (const TocEntry& e : toc) {
            while (!stack.empty() && e.level <= stack.back()) {
                body += "</navPoint>\n";
                stack.pop_back();
            }
            stack.push_back(e.level);
            depth = std::max(depth, int(stack.size()));
            ++order;
            char head[64];
            snprintf(head, sizeof head, "<navPoint id=\"np%d\" playOrder=\"%d\">", order, order);
            body += head;
            body += "<navLabel><text>" + (e.label.empty() ? std::string("-") : e.label) +
                    "</text></navLabel><content src=\"" + chapterName(e.chapter);
            if (e.id) body += "#h" + std::to_string(e.id);
            body += "\"/>\n";
        }
        while (!stack.empty()) {
            body += "</navPoint>\n";
            stack.pop_back();
        }
        s += "<meta name=\"dtb:depth\" content=\"" + std::to_string(depth) + "\"/>\n"
             "<meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
             "<meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n</head>\n"
             "<docTitle><text>" + esc(o_.title) + "</text></docTitle>\n<navMap>\n";
        z_.write(s);
        z_.write(body);
        z_.write("</navMap>\n</ncx>\n");
        z_.end();
    }
    void writeOpf(const char* uid) {
        z_.begin("OEBPS/content.opf");
        std::string s =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" "
            "version=\"2.0\">\n"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
            "xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
            "<dc:title>" + esc(o_.title) + "</dc:title>\n";
        if (!o_.author.empty())
            s += "<dc:creator opf:role=\"aut\">" + esc(o_.author) + "</dc:creator>\n";
        s += "<dc:language>" + esc(o_.language) + "</dc:language>\n"
             "<dc:identifier id=\"bookid\">" + std::string(uid) + "</dc:identifier>\n"
             "<meta name=\"generator\" content=\"" + esc(o_.generator) + "\"/>\n"
             "</metadata>\n<manifest>\n"
             "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
             "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
        z_.write(s);
        for (int i = 1; i <= chapters_; ++i) {
            char b[128];
            snprintf(b, sizeof b, "<item id=\"c%d\" href=\"%s\" media-type=\"application/xhtml+xml\"/>\n",
                     i, chapterName(i).c_str());
            z_.write(b);
        }
        z_.write("</manifest>\n<spine toc=\"ncx\">\n");
        for (int i = 1; i <= chapters_; ++i) {
            char b[48];
            snprintf(b, sizeof b, "<itemref idref=\"c%d\"/>\n", i);
            z_.write(b);
        }
        z_.write("</spine>\n</package>\n");
        z_.end();
    }

    Zip& z_;
    const Options& o_;
    bool open_ = false;
    int chapters_ = 0, blocks_ = 0, ids_ = 0;
    std::vector<TocEntry> toc_;
};

// ---------------------------------------------------------------------------
// Markdown inline elements
// ---------------------------------------------------------------------------

size_t runLen(const char* s, size_t n, size_t i) {
    size_t j = i;
    while (j < n && s[j] == s[i]) ++j;
    return j - i;
}

// Position of a backtick run of exactly r starting at or after i, or n.
size_t codeEnd(const char* s, size_t n, size_t i, size_t r) {
    while (i < n) {
        if (s[i] == '`') {
            const size_t k = runLen(s, n, i);
            if (k == r) return i;
            i += k;
        } else {
            ++i;
        }
    }
    return n;
}

// Index just past the ']' matching the '[' at i, or 0.
size_t bracketEnd(const char* s, size_t n, size_t i) {
    int depth = 0;
    for (size_t j = i; j < n; ++j) {
        if (s[j] == '\\') { ++j; continue; }
        if (s[j] == '`') {
            const size_t r = runLen(s, n, j);
            const size_t e = codeEnd(s, n, j + r, r);
            if (e < n) { j = e + r - 1; continue; }
            j += r - 1;
            continue;
        }
        if (s[j] == '[') ++depth;
        else if (s[j] == ']' && --depth == 0) return j + 1;
    }
    return 0;
}

bool linkable(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 ||
           url.compare(0, 7, "mailto:") == 0;
}

void inl(std::string& out, const char* s, size_t n, int depth, bool inLink = false);

// Emphasis closer for a run of delimiter c with length L (1-3) after `from`.
size_t findCloser(const char* s, size_t n, size_t from, char c, size_t L) {
    for (size_t j = from; j < n;) {
        const char d = s[j];
        if (d == '\\') { j += 2; continue; }
        if (d == '`') {
            const size_t r = runLen(s, n, j);
            const size_t e = codeEnd(s, n, j + r, r);
            j = e < n ? e + r : j + r;
            continue;
        }
        if (d == c) {
            const size_t r = runLen(s, n, j);
            const bool prevOk = !isSpace(uint8_t(s[j - 1])) && s[j - 1] != '\n';
            const bool nextOk = c != '_' || j + r >= n || !isAlnum(uint8_t(s[j + r]));
            const bool lenOk = c == '~' ? r == 2 : (r == L || r == 3);
            if (prevOk && nextOk && lenOk && j > from) return j + (r - L);
            j += r;
            continue;
        }
        ++j;
    }
    return std::string::npos;
}

void inl(std::string& out, const char* s, size_t n, int depth, bool inLink) {
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        const uint8_t prev = i ? uint8_t(s[i - 1]) : ' ';
        if (c == '\\' && i + 1 < n && isPunct(uint8_t(s[i + 1]))) {
            escapeTo(out, s + i + 1, 1);
            i += 2;
            continue;
        }
        if (c == '\n') {  // hard line break
            out += "<br/>";
            ++i;
            continue;
        }
        if (c == '`') {
            const size_t r = runLen(s, n, i);
            const size_t e = codeEnd(s, n, i + r, r);
            if (e < n) {
                size_t a = i + r, b = e;
                if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') { ++a; --b; }
                out += "<code>";
                escapeTo(out, s + a, b - a);
                out += "</code>";
                i = e + r;
            } else {
                out.append(r, '`');
                i += r;
            }
            continue;
        }
        if ((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) && depth < kMaxDepth) {
            const size_t b0 = c == '!' ? i + 1 : i;
            const size_t be = bracketEnd(s, n, b0);
            if (be && be < n && s[be] == '(') {
                int pd = 0;
                size_t k = be;
                for (; k < n; ++k) {
                    if (s[k] == '(') ++pd;
                    else if (s[k] == ')' && --pd == 0) break;
                }
                if (k < n) {
                    std::string url(s + be + 1, k - be - 1);
                    while (!url.empty() && url.front() == ' ') url.erase(0, 1);
                    const size_t sp = url.find(' ');
                    if (sp != std::string::npos) url.resize(sp);  // drop "title"
                    if (!url.empty() && url.front() == '<' && url.back() == '>')
                        url = url.substr(1, url.size() - 2);
                    const char* t = s + b0 + 1;
                    const size_t tn = be - b0 - 2;
                    if (c == '!') {  // images are not embedded: keep the alt text
                        out += '[';
                        inl(out, t, tn, depth + 1, inLink);
                        out += ']';
                    } else if (linkable(url) && !inLink) {
                        out += "<a href=\"";
                        escapeTo(out, url.data(), url.size(), true);
                        out += "\">";
                        inl(out, t, tn, depth + 1, true);
                        out += "</a>";
                    } else {
                        inl(out, t, tn, depth + 1, inLink);
                    }
                    i = k + 1;
                    continue;
                }
            }
            escapeTo(out, s + i, 1);
            ++i;
            continue;
        }
        if (c == '<') {
            const size_t e = std::string(s + i, n - i).find('>');
            if (e != std::string::npos && e > 1) {
                const std::string url(s + i + 1, e - 1);
                if (linkable(url) && url.find(' ') == std::string::npos) {
                    if (inLink) {
                        escapeTo(out, url.data(), url.size());
                        i += e + 1;
                        continue;
                    }
                    out += "<a href=\"";
                    escapeTo(out, url.data(), url.size(), true);
                    out += "\">";
                    escapeTo(out, url.data(), url.size());
                    out += "</a>";
                    i += e + 1;
                    continue;
                }
            }
            out += "&lt;";
            ++i;
            continue;
        }
        if ((c == '*' || c == '_' || c == '~') && depth < kMaxDepth) {
            const size_t r = runLen(s, n, i);
            const size_t L = c == '~' ? 2 : std::min<size_t>(r, 3);
            const bool nextOk = i + r < n && !isSpace(uint8_t(s[i + r]));
            const bool prevOk = c != '_' || !isAlnum(prev);
            if (nextOk && prevOk && (c != '~' || r == 2)) {
                const size_t e = findCloser(s, n, i + L, c, L);
                if (e != std::string::npos) {
                    static const char* const kOpen[] = {"", "<em>", "<strong>", "<strong><em>"};
                    static const char* const kClose[] = {"", "</em>", "</strong>", "</em></strong>"};
                    out += c == '~' ? "<del>" : kOpen[L];
                    inl(out, s + i + L, e - i - L, depth + 1, inLink);
                    out += c == '~' ? "</del>" : kClose[L];
                    i = e + L;
                    continue;
                }
            }
            out.append(r, c);
            i += r;
            continue;
        }
        if (c == 'h' && !inLink && !isAlnum(prev) &&
            (strncmp(s + i, "http://", std::min<size_t>(7, n - i)) == 0 ||
             strncmp(s + i, "https://", std::min<size_t>(8, n - i)) == 0) &&
            n - i > 8) {
            size_t e = i;
            while (e < n && !isSpace(uint8_t(s[e])) && s[e] != '\n' && s[e] != '<' && s[e] != ')')
                ++e;
            while (e > i && strchr(".,;:!?'\"", s[e - 1])) --e;
            const std::string url(s + i, e - i);
            out += "<a href=\"";
            escapeTo(out, url.data(), url.size(), true);
            out += "\">";
            escapeTo(out, url.data(), url.size());
            out += "</a>";
            i = e;
            continue;
        }
        // plain run
        size_t j = i + 1;
        while (j < n && !strchr("\\\n`[!<*_~h", s[j])) ++j;
        escapeTo(out, s + i, j - i);
        i = j;
    }
}

std::string inlineHtml(const std::string& s) {
    std::string o;
    inl(o, s.data(), s.size(), 0);
    return o;
}

// ---------------------------------------------------------------------------
// Markdown blocks
// ---------------------------------------------------------------------------

class Markdown {
public:
    explicit Markdown(Book& b) : b_(b) {}

    void line(const std::string& raw, bool cont) {
        if (cont) {  // continuation piece of an overlong line
            if (fence_) codeLine(raw);
            else appendRaw(raw);
            return;
        }
        if (fence_) {
            if (isFenceClose(raw)) fence_ = false;
            else codeLine(raw);
            return;
        }
        size_t i = 0;
        int indent = 0;
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) {
            indent += raw[i] == '\t' ? 4 : 1;
            ++i;
        }
        const std::string s = raw.substr(i);
        if (s.empty()) {
            flushPara();
            closeTable();
            closeQuote();
            blank_ = true;
            return;
        }
        const bool afterBlank = blank_;
        blank_ = false;

        // fenced code
        if (indent < 4 && (s.compare(0, 3, "```") == 0 || s.compare(0, 3, "~~~") == 0)) {
            flushAll();
            fence_ = true;
            fenceCh_ = s[0];
            fenceLen_ = runLen(s.data(), s.size(), 0);
            return;
        }
        // ATX heading
        if (s[0] == '#') {
            const size_t h = runLen(s.data(), s.size(), 0);
            if (h <= 6 && (h == s.size() || s[h] == ' ' || s[h] == '\t')) {
                std::string t = s.substr(h);
                while (!t.empty() && (t.back() == ' ' || t.back() == '#')) t.pop_back();
                while (!t.empty() && t.front() == ' ') t.erase(0, 1);
                flushAll();
                heading(int(h), t);
                return;
            }
        }
        // thematic break / setext underline
        if (s[0] == '-' || s[0] == '*' || s[0] == '_' || s[0] == '=') {
            size_t marks = 0;
            bool only = true;
            for (char ch : s) {
                if (ch == s[0]) ++marks;
                else if (ch != ' ' && ch != '\t') only = false;
            }
            if (only && (s[0] == '=' || s[0] == '-') && !para_.empty() && lists_.empty() &&
                !quote_ && !afterBlank) {
                const std::string t = para_;
                para_.clear();
                heading(s[0] == '=' ? 1 : 2, t);
                return;
            }
            if (only && marks >= 3 && s[0] != '=') {
                flushAll();
                b_.maybeSplit();
                b_.out("<hr/>\n");
                b_.block();
                return;
            }
        }
        // tables
        if (s[0] == '|') {
            if (!table_) {
                flushPara();
                closeLists();
                closeQuote();
            }
            tableRow(s);
            return;
        }
        closeTable();
        // block quotes
        if (s[0] == '>') {
            std::string t = s.substr(1);
            if (!t.empty() && t[0] == ' ') t.erase(0, 1);
            if (!quote_) {
                flushPara();
                closeLists();
                b_.maybeSplit();
                b_.out("<blockquote>\n");
                quote_ = true;
            }
            if (t.empty()) flushPara();
            else append(t);
            return;
        }
        if (quote_) {
            if (!para_.empty()) { append(s); return; }  // lazy continuation
            closeQuote();
        }
        // list items
        bool ordered = false;
        size_t m = listMarker(s, &ordered);
        if (m) {
            listItem(indent, ordered, s.substr(m));
            return;
        }
        if (!lists_.empty()) {
            if (!afterBlank || indent >= 2) {  // more text for the current item
                if (afterBlank) flushPara();
                append(s);
                return;
            }
            closeLists();
        }
        append(s);
    }

    void finish() { flushAll(); if (fence_) fence_ = false; }

private:
    struct List { int indent; bool ordered; };

    static size_t listMarker(const std::string& s, bool* ordered) {
        if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && (s.size() == 1 || s[1] == ' ' || s[1] == '\t')) {
            *ordered = false;
            return s.size() > 1 ? 2 : 1;
        }
        size_t j = 0;
        while (j < s.size() && j < 9 && s[j] >= '0' && s[j] <= '9') ++j;
        if (j && j < s.size() && (s[j] == '.' || s[j] == ')') &&
            (j + 1 == s.size() || s[j + 1] == ' ' || s[j + 1] == '\t')) {
            *ordered = true;
            return j + 1 < s.size() ? j + 2 : j + 1;
        }
        return 0;
    }

    bool isFenceClose(const std::string& raw) const {
        size_t i = 0;
        while (i < raw.size() && i < 4 && raw[i] == ' ') ++i;
        const size_t r = i < raw.size() && raw[i] == fenceCh_ ? runLen(raw.data(), raw.size(), i) : 0;
        if (r < fenceLen_) return false;
        for (size_t k = i + r; k < raw.size(); ++k)
            if (raw[k] != ' ' && raw[k] != '\t') return false;
        return true;
    }

    void codeLine(const std::string& s) {
        if (lists_.empty() && !quote_) b_.maybeSplit();
        std::string o = "<div class=\"code\">";
        if (s.empty()) o += "&#160;";
        else escapeTo(o, s.data(), s.size(), false, true);
        o += "</div>\n";
        b_.out(o);
        b_.block();
    }

    void append(std::string s) {
        // hard break: two trailing spaces or a trailing backslash
        bool hard = false;
        if (s.size() >= 2 && s[s.size() - 1] == ' ' && s[s.size() - 2] == ' ') hard = true;
        else if (!s.empty() && s.back() == '\\') { hard = true; s.pop_back(); }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        if (!para_.empty()) para_ += breakNext_ ? '\n' : ' ';
        breakNext_ = hard;
        appendRaw(s);
    }
    void appendRaw(const std::string& s) {
        para_ += s;
        if (para_.size() > kParaMax) flushPara();
    }

    void flushPara() {
        breakNext_ = false;
        if (para_.empty()) return;
        const std::string h = inlineHtml(para_);
        para_.clear();
        if (!lists_.empty()) {
            if (liText_) b_.out("<br/>");
            b_.out(h);
            liText_ = true;
            return;
        }
        if (quote_) quoteHasBlock_ = true;
        else b_.maybeSplit();
        b_.out("<p>" + h + "</p>\n");
        b_.block();
    }

    void heading(int level, const std::string& text) {
        const int id = b_.newId();
        if (level <= 2) b_.newChapter();
        const std::string h = inlineHtml(text);
        char open[40];
        snprintf(open, sizeof open, "<h%d id=\"h%d\">", level, id);
        b_.out(std::string(open) + h + "</h" + char('0' + level) + ">\n");
        b_.block();
        if (level <= 3) b_.addToc(level, id, stripTags(h));
    }

    void listItem(int indent, bool ordered, const std::string& text) {
        flushPara();
        closeTable();
        if (lists_.empty()) closeQuote();
        while (!lists_.empty() && indent < lists_.back().indent) closeOneList();
        if (!lists_.empty() && indent < lists_.back().indent + 2) {
            if (lists_.back().ordered != ordered) {
                closeOneList();
                openList(indent, ordered);
            } else {
                b_.out("</li>\n");
            }
        } else {
            openList(indent, ordered);
        }
        b_.out("<li>");
        liText_ = false;
        if (!text.empty()) append(text);
    }
    void openList(int indent, bool ordered) {
        if (lists_.empty()) b_.maybeSplit();
        b_.out(ordered ? "<ol>\n" : "<ul>\n");
        lists_.push_back(List{indent, ordered});
        if (lists_.size() == 1) b_.block();
    }
    void closeOneList() {
        flushPara();
        b_.out(lists_.back().ordered ? "</li>\n</ol>\n" : "</li>\n</ul>\n");
        lists_.pop_back();
        liText_ = true;
    }
    void closeLists() {
        while (!lists_.empty()) closeOneList();
    }
    void closeQuote() {
        if (!quote_) return;
        const bool wasEmpty = para_.empty() && !quoteHasBlock_;
        flushPara();
        if (wasEmpty) b_.out("<p>&#160;</p>\n");
        b_.out("</blockquote>\n");
        b_.block();
        quote_ = false;
        quoteHasBlock_ = false;
    }

    // ---- tables ----
    static std::vector<std::string> cells(const std::string& s) {
        std::vector<std::string> v;
        std::string cur;
        size_t i = s[0] == '|' ? 1 : 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') { cur += '|'; ++i; continue; }
            if (s[i] == '|') { v.push_back(cur); cur.clear(); continue; }
            cur += s[i];
        }
        size_t a = 0;
        while (a < cur.size() && cur[a] == ' ') ++a;
        if (a < cur.size()) v.push_back(cur);
        for (std::string& c : v) {
            size_t x = 0, y = c.size();
            while (x < y && (c[x] == ' ' || c[x] == '\t')) ++x;
            while (y > x && (c[y - 1] == ' ' || c[y - 1] == '\t')) --y;
            c = c.substr(x, y - x);
        }
        return v;
    }
    static bool isSeparator(const std::string& s) {
        bool dash = false;
        for (char c : s) {
            if (c == '-') dash = true;
            else if (c != '|' && c != ':' && c != ' ' && c != '\t') return false;
        }
        return dash;
    }
    void emitRow(const std::vector<std::string>& v, bool header) {
        std::string o = "<tr>";
        for (const std::string& c : v) {
            o += header ? "<th>" : "<td>";
            o += inlineHtml(c);
            o += header ? "</th>" : "</td>";
        }
        if (v.empty()) o += "<td></td>";
        o += "</tr>\n";
        b_.out(o);
    }
    void tableRow(const std::string& s) {
        if (!table_) {
            table_ = true;
            pendingRow_ = s;
            havePending_ = true;
            return;
        }
        if (havePending_) {
            havePending_ = false;
            b_.maybeSplit();
            b_.out("<table>\n");
            b_.block();
            if (isSeparator(s)) {
                emitRow(cells(pendingRow_), true);
                return;
            }
            emitRow(cells(pendingRow_), false);
        }
        if (!isSeparator(s)) emitRow(cells(s), false);
    }
    void closeTable() {
        if (!table_) return;
        if (havePending_) {
            havePending_ = false;
            b_.out("<table>\n");
            b_.block();
            emitRow(cells(pendingRow_), false);
        }
        b_.out("</table>\n");
        table_ = false;
    }

    void flushAll() {
        flushPara();
        closeTable();
        closeLists();
        closeQuote();
    }

    Book& b_;
    std::string para_;
    bool breakNext_ = false, blank_ = false;
    std::vector<List> lists_;
    bool liText_ = false;
    bool quote_ = false, quoteHasBlock_ = false;
    bool fence_ = false;
    char fenceCh_ = '`';
    size_t fenceLen_ = 3;
    bool table_ = false, havePending_ = false;
    std::string pendingRow_;
};

}  // namespace

// ---------------------------------------------------------------------------

Kind kindForPath(const std::string& path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return Kind::Text;
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (e == "md" || e == "markdown" || e == "mdown" || e == "mkd") return Kind::Markdown;
    static const char* const kCode[] = {"c",   "cc",  "cpp", "cxx", "h",    "hh",   "hpp",  "hxx",
                                        "ino", "py",  "pyw", "xml", "urdf", "xacro", "sdf", "svg",
                                        "html", "htm", "js",  "json", "sh",  "cmake", "ini", "yaml",
                                        "yml", "toml", "lua", "rs",  "java", "ts",   "css"};
    for (const char* k : kCode)
        if (e == k) return Kind::Code;
    return Kind::Text;
}

std::string guessTitle(const Reader& rd, uint32_t size, Kind kind, const std::string& fallback) {
    if (kind == Kind::Markdown) {
        LineReader lr(rd, std::min<uint32_t>(size, 16 * 1024));
        std::string line;
        bool more = false, fence = false;
        while (lr.next(line, &more)) {
            if (line.compare(0, 3, "```") == 0) fence = !fence;
            if (!fence && line.size() > 2 && line[0] == '#' && line[1] == ' ') {
                std::string t = line.substr(2);
                while (!t.empty() && (t.back() == ' ' || t.back() == '#')) t.pop_back();
                // plain text version of the inline markup
                std::string h = stripTags(inlineHtml(t)), o;
                for (size_t i = 0; i < h.size();) {  // unescape the few entities we produce
                    if (h.compare(i, 5, "&amp;") == 0) { o += '&'; i += 5; }
                    else if (h.compare(i, 4, "&lt;") == 0) { o += '<'; i += 4; }
                    else if (h.compare(i, 4, "&gt;") == 0) { o += '>'; i += 4; }
                    else o += h[i++];
                }
                if (!o.empty()) return o;
            }
        }
    }
    return fallback;
}

std::string safeFileName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) o += '-';
        else o += char(c);
    }
    size_t a = 0, b = o.size();
    while (a < b && (o[a] == ' ' || o[a] == '.')) ++a;
    while (b > a && (o[b - 1] == ' ' || o[b - 1] == '.')) --b;
    o = o.substr(a, b - a);
    if (o.size() > 80) {
        size_t k = 80;
        while (k > 0 && (uint8_t(o[k]) & 0xC0) == 0x80) --k;
        o.resize(k);
    }
    return o.empty() ? std::string("untitled") : o;
}

bool write(const Reader& rd, uint32_t size, const Options& opt, const std::string& outPath,
           std::string* err, const Progress& progress) {
    Zip z;
    if (!z.open(outPath)) {
        if (err) *err = z.error();
        return false;
    }
    // mimetype must come first, stored, without extra fields
    z.begin("mimetype");
    z.write("application/epub+zip");
    z.end();
    z.begin("META-INF/container.xml");
    z.write(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    z.end();

    Book book(z, opt);
    LineReader lr(rd, size);
    Markdown md(book);
    std::string line;
    bool more = false, cont = false;
    uint64_t h1 = fnv(0xcbf29ce484222325ull, opt.title.data(), opt.title.size());
    uint64_t h2 = fnv(0x84222325cbf29ce4ull, opt.author.data(), opt.author.size());
    uint32_t hashed = 0, lastProgress = 0;
    bool cancelled = false;

    while (z.ok() && lr.next(line, &more)) {
        if (hashed < 4096) {
            h1 = fnv(h1, line.data(), line.size());
            h2 = fnv(h2 ^ h1, line.data(), line.size());
            hashed += uint32_t(line.size()) + 1;
        }
        switch (opt.kind) {
            case Kind::Markdown:
                md.line(line, cont);
                break;
            case Kind::Code: {
                if (!cont) book.maybeSplit();
                std::string o;
                if (!cont) o = "<div class=\"code\">";
                if (line.empty() && !cont && !more) o += "&#160;";
                escapeTo(o, line.data(), line.size(), false, true);
                if (!more) {
                    o += "</div>\n";
                    book.block();
                }
                book.out(o);
                break;
            }
            case Kind::Text: {
                if (!cont && line.empty()) break;
                if (!cont) book.maybeSplit();
                std::string o;
                if (!cont) o = "<p>";
                escapeTo(o, line.data(), line.size());
                if (!more) {
                    o += "</p>\n";
                    book.block();
                }
                book.out(o);
                break;
            }
        }
        cont = more;
        const uint32_t p = lr.position();
        if (progress && p - lastProgress >= 4096) {
            lastProgress = p;
            if (!progress(p, size)) {
                cancelled = true;
                break;
            }
        }
    }
    if (opt.kind == Kind::Markdown) md.finish();
    bool ok = !cancelled && z.ok() && book.finish(h1, h2) && z.finish();
    if (!ok) {
        if (err) *err = cancelled ? "Cancelled" : z.error();
        z.close();
        fs::remove(outPath.c_str());
    }
    return ok;
}

}  // namespace pd::epub
