// Streaming syntax highlighter for Markdown, XML, Python and C/C++.
//
// The lexer classifies bytes into colour classes. It is fed the document as a
// sequence of contiguous chunks and never needs the whole file: all state that
// crosses chunk or line boundaries (block comments, triple-quoted strings,
// fenced code, multi-line XML tags...) lives in a small copyable LexState.
// The UI snapshots that state at the top of the screen, so only the visible
// rows plus a bounded back-scan are ever lexed.
//
// The lexer is pure (no hardware, no TextBuffer): it is unit-tested on the host.
#pragma once
#include <cstdint>

namespace pd::syn {

enum Lang : uint8_t { None = 0, Markdown, Xml, Python, Cpp, kLangCount };

// Colour classes. Themes provide one colour per class (Plain = theme fg).
enum Cls : uint8_t {
    Plain = 0,
    Keyword,  // language keywords
    Type,     // built-in types / builtins, def/class names
    String,
    Number,
    Comment,
    Meta,     // preprocessor, decorators, XML prolog / PI / entities, fence info
    Tag,      // XML tag names
    Attr,     // XML attribute names
    Punct,    // markup punctuation: < > / = in XML, Markdown markers
    Heading,  // Markdown headings
    Emph,     // Markdown *emphasis* / **strong**
    Code,     // Markdown inline code and fenced blocks
    Link,     // Markdown link text, URLs
    kClsCount
};

// How far back (bytes) the UI re-lexes to recover the state at the top of the
// screen. Constructs opened further back than this may be shown wrongly until
// the view scrolls closer to them.
constexpr uint32_t kBackScan = 8 * 1024;
// Bytes of lookahead the caller should make available past each chunk.
constexpr uint32_t kLookahead = 64;

Lang langForPath(const char* path);  // by extension, case-insensitive
const char* langName(Lang l);        // short tag for the status bar ("" for None)

struct LexState {
    uint8_t lang = None;
    uint8_t mode = 0;      // multi-byte construct in progress (see syntax.cpp)
    uint8_t quote = 0;     // closing quote / fence character
    uint8_t count = 0;     // fence length, backtick run, raw-string delimiter length
    uint8_t prev = '\n';   // previous byte
    uint8_t remCls = 0;    // class of rem pending bytes
    uint8_t flags = 0;     // per-line and misc flags (see syntax.cpp)
    uint8_t linkPart = 0;  // Markdown link: which part is being emitted
    uint32_t rem = 0;      // bytes already classified by a token that started earlier
    uint16_t partLen = 0;  // Markdown link: bytes left in the current part
    uint16_t partLen2 = 0; // Markdown link: length of the URL part
    char raw[16] = {};     // C++ raw string delimiter
};

class Lexer {
public:
    // Start of a document (or of a back-scan, which assumes a line start).
    void reset(Lang lang);
    Lang lang() const { return Lang(st_.lang); }

    // Classifies s[0, n) into out[0, n) (out may be null to only advance the
    // state). s[n, avail) is lookahead: it must be the bytes that follow in the
    // document, and is only peeked at. The next call must start at s + n.
    void run(const uint8_t* s, uint32_t n, uint32_t avail, uint8_t* out);

    const LexState& state() const { return st_; }
    void setState(const LexState& s) { st_ = s; }

private:
    struct Tok { uint8_t cls; uint32_t len; };
    Tok next(const uint8_t* s, uint32_t avail);
    Tok nextCpp(const uint8_t* s, uint32_t avail);
    Tok nextPython(const uint8_t* s, uint32_t avail);
    Tok nextXml(const uint8_t* s, uint32_t avail);
    Tok nextMarkdown(const uint8_t* s, uint32_t avail);
    Tok mdInline(const uint8_t* s, uint32_t avail);
    Tok word(const uint8_t* s, uint32_t avail);
    Tok number(const uint8_t* s, uint32_t avail);
    Tok scanString(const uint8_t* s, uint32_t avail, bool multiline);
    Tok scanUntil(const uint8_t* s, uint32_t avail, const char* end, uint8_t cls);
    void endOfLine();

    LexState st_;
    bool keepBol_ = false;  // current token does not end the line's leading-blank region
};

}  // namespace pd::syn
