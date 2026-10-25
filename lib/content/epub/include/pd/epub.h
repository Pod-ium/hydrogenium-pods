// ePub export: turns the document into an EPUB 2.0.1 file on the SD card.
//
// The document is streamed (never loaded into RAM) and converted by kind:
//   Markdown  headings, emphasis, code, links, lists, quotes, tables, rules;
//             chapters start at # and ## headings, which also build the TOC
//   Text      one paragraph per line (the editor soft-wraps, so a line is a
//             paragraph)
//   Code      one line per block, indentation kept with no-break spaces
// Chapters are also split every ~48 KB so e-readers with little RAM (the
// ESP32-C3 in the Xteink readers) never get a huge XHTML file. The zip uses
// the "stored" method; entries are written in one pass and their headers are
// patched afterwards, so no temporary files are needed.
//
// Pure C++ on top of pd::fs: unit-tested on the host.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace pd::epub {

enum class Kind { Text, Markdown, Code };

struct Options {
    std::string title;
    std::string author;          // may be empty
    std::string language = "en"; // BCP 47, e.g. "en", "it"
    std::string generator = "PicoPods";  // OPF generator metadata (the pod name)
    Kind kind = Kind::Text;
};

// Reads document bytes [pos, pos + n) into buf; returns bytes read.
using Reader = std::function<uint32_t(uint32_t pos, uint8_t* buf, uint32_t n)>;
// Returns false to cancel. Arguments: bytes converted / document size.
using Progress = std::function<bool(uint32_t done, uint32_t total)>;

Kind kindForPath(const std::string& path);

// Title suggestion: the first "# " heading (Markdown, first 16 KB), else fallback.
std::string guessTitle(const Reader& rd, uint32_t size, Kind kind, const std::string& fallback);

// A file name that is safe on FAT and on the reader: no path separators or
// reserved characters, trimmed, never empty.
std::string safeFileName(const std::string& s);

// Writes the book to outPath (replacing it). On failure the partial file is
// removed and *err explains why.
bool write(const Reader& rd, uint32_t size, const Options& opt, const std::string& outPath,
           std::string* err, const Progress& progress = nullptr);

}  // namespace pd::epub
