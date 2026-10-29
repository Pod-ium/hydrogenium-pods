#include "syntax.h"

#include <cstring>

namespace syn {
namespace {

// ---- modes (LexState::mode) ------------------------------------------------
enum Mode : uint8_t {
    M_NORMAL = 0,
    M_LINE,          // rest of the line in class LexState::count
    M_LINECOMMENT,   // // or # comment
    M_BLOCKCOMMENT,  // /* ... */
    M_STR,           // "..." or '...' ending at the quote or the line end
    M_TRIPLE,        // Python """...""" / '''...'''
    M_RAW,           // C++ R"delim(...)delim"
    M_ANGLE,         // C++ #include <...>
    M_XCOMMENT,      // <!-- ... -->  (XML and Markdown)
    M_XCDATA,        // <![CDATA[ ... ]]>
    M_XPI,           // <? ... ?>
    M_XDECL,         // <!DOCTYPE ... >
    M_XTAG,          // inside < ... >
    M_XATTRVAL,      // quoted attribute value inside a tag
    M_FENCEOPEN,     // Markdown: info string after an opening ``` fence
    M_FENCE,         // Markdown: fenced code body
    M_MDCODE,        // Markdown: inline `code`
    M_MDLINK,        // Markdown: [text](url)
};

// ---- flags (LexState::flags) ------------------------------------------------
enum : uint8_t {
    F_BOL = 1,        // only blanks so far on this line
    F_PP = 2,         // C++: preprocessor line
    F_INCL = 4,       // C++: after #include, '<' opens a header name
    F_DEFNAME = 8,    // Python: next identifier is a def/class name
    F_EM = 16,        // Markdown *em*
    F_STRONG = 32,    // Markdown **strong**
    F_ESC = 64,       // backslash pending inside a string
    F_TAGNAME = 128,  // XML: next name inside the tag is the tag name
};

// ---- keyword tables (sorted bytewise; checked by the host tests) ----------
const char* const kCppKw[] = {
    "NULL", "alignas", "alignof", "asm", "auto", "break", "case", "catch", "class", "co_await",
    "co_return", "co_yield", "concept", "const", "const_cast", "consteval", "constexpr",
    "constinit", "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else",
    "enum", "explicit", "export", "extern", "false", "final", "for", "friend", "goto", "if",
    "inline", "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "override",
    "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
    "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "using",
    "virtual", "volatile", "while"};
const char* const kCppTypes[] = {
    "bool", "char", "char16_t", "char32_t", "char8_t", "double", "float", "int", "int16_t",
    "int32_t", "int64_t", "int8_t", "intptr_t", "long", "map", "ptrdiff_t", "shared_ptr",
    "short", "signed", "size_t", "ssize_t", "std", "string", "uint16_t", "uint32_t", "uint64_t",
    "uint8_t", "uintptr_t", "unique_ptr", "unsigned", "vector", "void", "wchar_t"};
const char* const kPyKw[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "case", "class",
    "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global",
    "if", "import", "in", "is", "lambda", "match", "nonlocal", "not", "or", "pass", "raise",
    "return", "try", "while", "with", "yield"};
const char* const kPyBuiltins[] = {
    "AttributeError", "Exception", "IndexError", "KeyError", "NotImplementedError", "OSError",
    "RuntimeError", "StopIteration", "TypeError", "ValueError", "__init__", "__main__",
    "__name__", "abs", "all", "any", "bool", "bytes", "cls", "dict", "enumerate", "filter",
    "float", "getattr", "hasattr", "hash", "id", "input", "int", "isinstance", "issubclass",
    "iter", "len", "list", "map", "max", "min", "next", "object", "open", "print", "range",
    "repr", "reversed", "round", "self", "set", "setattr", "sorted", "str", "sum", "super",
    "tuple", "type", "zip"};

template <size_t N>
bool inTable(const char* const (&tab)[N], const uint8_t* w, uint32_t len) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const char* k = tab[mid];
        const uint32_t kl = uint32_t(strlen(k));
        int r = memcmp(w, k, len < kl ? len : kl);
        if (r == 0) r = int(len) - int(kl);
        if (r == 0) return true;
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

inline bool isBlank(uint8_t c) { return c == ' ' || c == '\t' || c == '\r'; }
inline bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(uint8_t c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
// Identifier bytes; UTF-8 bytes count as letters.
inline bool isIdent(uint8_t c) { return isAlpha(c) || isDigit(c) || c == '_' || c >= 0x80; }
inline bool isAlnum(uint8_t c) { return isAlpha(c) || isDigit(c) || c >= 0x80; }

inline bool startsWith(const uint8_t* s, uint32_t avail, const char* lit) {
    const uint32_t n = uint32_t(strlen(lit));
    return avail >= n && memcmp(s, lit, n) == 0;
}

inline uint32_t blankRun(const uint8_t* s, uint32_t avail) {
    uint32_t j = 0;
    while (j < avail && isBlank(s[j])) ++j;
    return j;
}

inline uint32_t toEol(const uint8_t* s, uint32_t avail) {
    const void* nl = memchr(s, '\n', avail);
    return nl ? uint32_t(static_cast<const uint8_t*>(nl) - s) : avail;
}

}  // namespace

// ---------------------------------------------------------------------------

Lang langForPath(const char* path) {
    if (!path) return None;
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return None;
    char ext[12];
    size_t n = 0;
    for (const char* p = dot + 1; *p && n < sizeof ext - 1; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
        ext[n++] = c;
    }
    ext[n] = 0;
    static const struct { const char* ext; Lang lang; } kMap[] = {
        {"md", Markdown},  {"markdown", Markdown}, {"mdown", Markdown}, {"mkd", Markdown},
        {"xml", Xml},      {"svg", Xml},           {"urdf", Xml},       {"xacro", Xml},
        {"sdf", Xml},      {"launch", Xml},        {"ini_xml", Xml},    {"xsd", Xml},
        {"xsl", Xml},      {"xslt", Xml},          {"html", Xml},       {"htm", Xml},
        {"gpx", Xml},      {"kml", Xml},           {"py", Python},      {"pyw", Python},
        {"pyi", Python},   {"c", Cpp},             {"cc", Cpp},         {"cpp", Cpp},
        {"cxx", Cpp},      {"c++", Cpp},           {"h", Cpp},          {"hh", Cpp},
        {"hpp", Cpp},      {"hxx", Cpp},           {"h++", Cpp},        {"ino", Cpp},
        {"inl", Cpp},      {"ipp", Cpp},           {"tpp", Cpp},
    };
    for (const auto& m : kMap)
        if (!strcmp(ext, m.ext)) return m.lang;
    return None;
}

const char* langName(Lang l) {
    switch (l) {
        case Markdown: return "MD";
        case Xml: return "XML";
        case Python: return "PY";
        case Cpp: return "C++";
        default: return "";
    }
}

void Lexer::reset(Lang lang) {
    st_ = LexState();
    st_.lang = lang;
    st_.flags = F_BOL;
}

void Lexer::run(const uint8_t* s, uint32_t n, uint32_t avail, uint8_t* out) {
    if (avail < n) avail = n;
    uint32_t i = 0;
    while (i < n) {
        if (st_.rem) {
            const uint32_t k = st_.rem < n - i ? st_.rem : n - i;
            if (out) memset(out + i, st_.remCls, k);
            st_.rem -= k;
            i += k;
            continue;
        }
        Tok t = next(s + i, avail - i);
        const uint32_t k = t.len < n - i ? t.len : n - i;
        if (out) memset(out + i, t.cls, k);
        if (t.len > k) {
            st_.rem = t.len - k;
            st_.remCls = t.cls;
        }
        i += k;
    }
}

void Lexer::endOfLine() {
    const bool cont = st_.prev == '\\' && (st_.lang == Cpp || st_.lang == Python);
    switch (st_.mode) {
        case M_LINE:
        case M_LINECOMMENT:
        case M_ANGLE:
        case M_MDCODE:
        case M_MDLINK:
            st_.mode = M_NORMAL;
            break;
        case M_STR:
            if (!cont) st_.mode = M_NORMAL;
            break;
        case M_FENCEOPEN:
            st_.mode = M_FENCE;
            break;
        default:
            break;
    }
    uint8_t keep = 0;
    if ((st_.flags & F_PP) && cont) keep |= F_PP;
    if (st_.mode == M_STR || st_.mode == M_TRIPLE || st_.mode == M_XATTRVAL)
        keep |= st_.flags & F_ESC;
    if (st_.mode == M_XTAG) keep |= st_.flags & F_TAGNAME;
    st_.flags = uint8_t(F_BOL | keep);
    st_.linkPart = 0;
}

Lexer::Tok Lexer::next(const uint8_t* s, uint32_t avail) {
    if (s[0] == '\n') {
        endOfLine();
        st_.prev = '\n';
        return {Plain, 1};
    }
    keepBol_ = false;
    Tok t;
    switch (st_.mode) {
        case M_LINE: t = {st_.count, toEol(s, avail)}; break;
        case M_LINECOMMENT: t = {Comment, toEol(s, avail)}; break;
        case M_BLOCKCOMMENT: t = scanUntil(s, avail, "*/", Comment); break;
        case M_STR: t = scanString(s, avail, false); break;
        case M_TRIPLE: t = scanString(s, avail, true); break;
        case M_XATTRVAL: t = scanString(s, avail, true); break;
        case M_RAW: {
            char end[20];
            end[0] = ')';
            memcpy(end + 1, st_.raw, st_.count);
            end[1 + st_.count] = '"';
            end[2 + st_.count] = 0;
            t = scanUntil(s, avail, end, String);
            break;
        }
        case M_ANGLE: t = scanUntil(s, avail, ">", String); break;
        case M_XCOMMENT: t = scanUntil(s, avail, "-->", Comment); break;
        case M_XCDATA: t = scanUntil(s, avail, "]]>", String); break;
        case M_XPI: t = scanUntil(s, avail, "?>", Meta); break;
        case M_XDECL: t = scanUntil(s, avail, ">", Meta); break;
        default:
            switch (st_.lang) {
                case Cpp: t = nextCpp(s, avail); break;
                case Python: t = nextPython(s, avail); break;
                case Xml: t = nextXml(s, avail); break;
                case Markdown: t = nextMarkdown(s, avail); break;
                default: t = {Plain, toEol(s, avail)}; break;
            }
    }
    if (t.len == 0) t.len = 1;
    if (t.len > avail) t.len = avail;
    if (!keepBol_) {
        for (uint32_t j = 0; j < t.len; ++j) {
            if (!isBlank(s[j])) { st_.flags &= uint8_t(~F_BOL); break; }
        }
    }
    st_.prev = s[t.len - 1];
    return t;
}

// Consumes up to and including `end`, stopping before a line break. When the
// terminator is found the mode returns to normal.
Lexer::Tok Lexer::scanUntil(const uint8_t* s, uint32_t avail, const char* end, uint8_t cls) {
    const uint32_t el = uint32_t(strlen(end));
    uint32_t j = 0;
    while (j < avail) {
        const uint8_t c = s[j];
        if (c == '\n') return {cls, j};
        if (c == uint8_t(end[0])) {
            if (j + el > avail) break;  // cannot decide yet: stop before it
            if (memcmp(s + j, end, el) == 0) {
                st_.mode = M_NORMAL;
                return {cls, j + el};
            }
        }
        ++j;
    }
    return {cls, j ? j : 1};
}

// Strings: st_.quote is the quote, st_.count is 3 for triple-quoted.
Lexer::Tok Lexer::scanString(const uint8_t* s, uint32_t avail, bool multiline) {
    (void)multiline;
    const bool escapes = st_.lang != Xml;
    const uint8_t q = st_.quote;
    const uint32_t ql = st_.count == 3 ? 3 : 1;
    uint32_t j = 0;
    while (j < avail) {
        const uint8_t c = s[j];
        if (c == '\n') return {String, j};
        if (st_.flags & F_ESC) {
            st_.flags &= uint8_t(~F_ESC);
            ++j;
            continue;
        }
        if (escapes && c == '\\') {
            st_.flags |= F_ESC;
            ++j;
            continue;
        }
        if (c == q) {
            if (ql == 1 || (j + 3 <= avail && s[j + 1] == q && s[j + 2] == q)) {
                st_.mode = st_.mode == M_XATTRVAL ? M_XTAG : M_NORMAL;
                return {String, j + ql};
            }
            if (j + 3 > avail && j > 0) return {String, j};
        }
        ++j;
    }
    return {String, j ? j : 1};
}

Lexer::Tok Lexer::word(const uint8_t* s, uint32_t avail) {
    uint32_t j = 0;
    while (j < avail && isIdent(s[j])) ++j;
    return {Plain, j};
}

Lexer::Tok Lexer::number(const uint8_t* s, uint32_t avail) {
    uint32_t j = 1;
    while (j < avail) {
        const uint8_t c = s[j];
        if (isAlnum(c) || c == '_' || c == '.' || (c == '\'' && st_.lang == Cpp && j + 1 < avail &&
                                                   isAlnum(s[j + 1]))) {
            ++j;
            continue;
        }
        const uint8_t p = s[j - 1] | 0x20;
        const bool hex = avail > 1 && (s[1] | 0x20) == 'x';
        if ((c == '+' || c == '-') && ((p == 'e' && !hex) || p == 'p')) { ++j; continue; }
        break;
    }
    return {Number, j};
}

// ---------------------------------------------------------------------------
// C / C++
// ---------------------------------------------------------------------------

Lexer::Tok Lexer::nextCpp(const uint8_t* s, uint32_t avail) {
    const uint8_t c = s[0];
    const uint8_t c1 = avail > 1 ? s[1] : 0;
    if (isBlank(c)) return {Plain, blankRun(s, avail)};
    if ((st_.flags & F_BOL) && c == '#') {
        uint32_t j = 1 + blankRun(s + 1, avail - 1);
        const uint32_t w = j;
        while (j < avail && isIdent(s[j])) ++j;
        const uint32_t wl = j - w;
        if ((wl == 7 && !memcmp(s + w, "include", 7)) || (wl == 6 && !memcmp(s + w, "import", 6)) ||
            (wl == 12 && !memcmp(s + w, "include_next", 12)))
            st_.flags |= F_INCL;
        st_.flags |= F_PP;
        return {Meta, j};
    }
    if (c == '/' && c1 == '/') { st_.mode = M_LINECOMMENT; return {Comment, 2}; }
    if (c == '/' && c1 == '*') { st_.mode = M_BLOCKCOMMENT; return {Comment, 2}; }
    if (c == '"' || c == '\'') {
        st_.mode = M_STR;
        st_.quote = c;
        st_.count = 1;
        st_.flags &= uint8_t(~F_ESC);
        return {String, 1};
    }
    if (c == '<' && (st_.flags & F_INCL)) {
        st_.flags &= uint8_t(~F_INCL);
        st_.mode = M_ANGLE;
        return {String, 1};
    }
    if (isIdent(st_.prev) && isIdent(c)) return word(s, avail);  // tail of a split word
    if (isDigit(c) || (c == '.' && isDigit(c1))) return number(s, avail);
    if (isIdent(c)) {
        Tok w = word(s, avail);
        const uint32_t n = w.len;
        const uint8_t after = n < avail ? s[n] : 0;
        const bool rawPrefix = n <= 3 && s[n - 1] == 'R' &&
                               (n == 1 || (n == 2 && (s[0] == 'L' || s[0] == 'u' || s[0] == 'U')) ||
                                (n == 3 && s[0] == 'u' && s[1] == '8'));
        if (rawPrefix && after == '"') {
            uint32_t j = n + 1, d = 0;
            while (j < avail && d < sizeof st_.raw && s[j] != '(' && s[j] != '\n' && !isBlank(s[j]) &&
                   s[j] != ')' && s[j] != '\\') {
                ++j;
                ++d;
            }
            if (j < avail && s[j] == '(') {
                memcpy(st_.raw, s + n + 1, d);
                st_.count = uint8_t(d);
                st_.mode = M_RAW;
                return {String, j + 1};
            }
        }
        const bool strPrefix = (n == 1 && (s[0] == 'L' || s[0] == 'u' || s[0] == 'U')) ||
                               (n == 2 && s[0] == 'u' && s[1] == '8');
        if (strPrefix && (after == '"' || after == '\'')) return {String, n};
        if (inTable(kCppKw, s, n)) return {Keyword, n};
        if (inTable(kCppTypes, s, n)) return {Type, n};
        return w;
    }
    return {Plain, 1};
}

// ---------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------

Lexer::Tok Lexer::nextPython(const uint8_t* s, uint32_t avail) {
    const uint8_t c = s[0];
    if (isBlank(c)) return {Plain, blankRun(s, avail)};
    if (c == '#') { st_.mode = M_LINECOMMENT; return {Comment, 1}; }
    if (!isIdent(st_.prev) || c == '"' || c == '\'') {
        // String, with up to two prefix letters (r, b, u, f in any case).
        uint32_t k = 0;
        while (k < 2 && k < avail && strchr("rRbBuUfF", s[k]) && s[k]) ++k;
        if (k < avail && (s[k] == '"' || s[k] == '\'')) {
            const uint8_t q = s[k];
            st_.quote = q;
            st_.flags &= uint8_t(~F_ESC);
            if (k + 2 < avail && s[k + 1] == q && s[k + 2] == q) {
                st_.mode = M_TRIPLE;
                st_.count = 3;
                return {String, k + 3};
            }
            st_.mode = M_STR;
            st_.count = 1;
            return {String, k + 1};
        }
    }
    if ((st_.flags & F_BOL) && c == '@') {
        uint32_t j = 1;
        while (j < avail && (isIdent(s[j]) || s[j] == '.')) ++j;
        return {Meta, j};
    }
    if (isIdent(st_.prev) && isIdent(c)) return word(s, avail);
    if (isDigit(c) || (c == '.' && avail > 1 && isDigit(s[1]))) return number(s, avail);
    if (isIdent(c)) {
        const Tok w = word(s, avail);
        if (st_.prev == '.') return w;  // attribute access: obj.match, re.compile
        if (st_.flags & F_DEFNAME) {
            st_.flags &= uint8_t(~F_DEFNAME);
            return {Tag, w.len};
        }
        if (inTable(kPyKw, s, w.len)) {
            if ((w.len == 3 && !memcmp(s, "def", 3)) || (w.len == 5 && !memcmp(s, "class", 5)))
                st_.flags |= F_DEFNAME;
            return {Keyword, w.len};
        }
        if (inTable(kPyBuiltins, s, w.len)) return {Type, w.len};
        return w;
    }
    st_.flags &= uint8_t(~F_DEFNAME);
    return {Plain, 1};
}

// ---------------------------------------------------------------------------
// XML (also good enough for HTML)
// ---------------------------------------------------------------------------

Lexer::Tok Lexer::nextXml(const uint8_t* s, uint32_t avail) {
    const uint8_t c = s[0];
    if (st_.mode == M_XTAG) {
        if (isBlank(c)) return {Plain, blankRun(s, avail)};
        if (c == '/' && avail > 1 && s[1] == '>') {
            st_.mode = M_NORMAL;
            return {Punct, 2};
        }
        if (c == '>' || c == '?' || c == '/') {
            if (c == '>') st_.mode = M_NORMAL;
            return {Punct, 1};
        }
        if (c == '=') return {Punct, 1};
        if (c == '"' || c == '\'') {
            st_.mode = M_XATTRVAL;
            st_.quote = c;
            st_.count = 1;
            st_.flags &= uint8_t(~F_TAGNAME);
            return {String, 1};
        }
        if (c == '<') {  // malformed: a new tag starts
            st_.flags |= F_TAGNAME;
            return {Punct, 1};
        }
        uint32_t j = 0;
        while (j < avail && !isBlank(s[j]) && s[j] != '\n' && !strchr("/>=\"'<", s[j])) ++j;
        const bool name = st_.flags & F_TAGNAME;
        st_.flags &= uint8_t(~F_TAGNAME);
        return {name ? Tag : Attr, j ? j : 1};
    }
    if (c == '<') {
        if (startsWith(s, avail, "<!--")) { st_.mode = M_XCOMMENT; return {Comment, 4}; }
        if (startsWith(s, avail, "<![CDATA[")) { st_.mode = M_XCDATA; return {Meta, 9}; }
        if (startsWith(s, avail, "<?")) { st_.mode = M_XPI; return {Meta, 2}; }
        if (startsWith(s, avail, "<!")) { st_.mode = M_XDECL; return {Meta, 2}; }
        st_.mode = M_XTAG;
        st_.flags |= F_TAGNAME;
        return {Punct, uint32_t(avail > 1 && s[1] == '/' ? 2 : 1)};
    }
    if (c == '&') {
        uint32_t j = 1;
        while (j < avail && j < 12 && (isAlnum(s[j]) || s[j] == '#')) ++j;
        if (j < avail && s[j] == ';' && j > 1) return {Meta, j + 1};
        return {Plain, 1};
    }
    uint32_t j = 0;
    while (j < avail && s[j] != '<' && s[j] != '&' && s[j] != '\n') ++j;
    return {Plain, j};
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

Lexer::Tok Lexer::nextMarkdown(const uint8_t* s, uint32_t avail) {
    const uint8_t c = s[0];
    const bool bol = st_.flags & F_BOL;

    if (st_.mode == M_FENCEOPEN) return {Meta, toEol(s, avail)};
    if (st_.mode == M_FENCE) {
        if (bol && isBlank(c)) {
            keepBol_ = true;
            return {Code, blankRun(s, avail)};
        }
        if (bol && c == st_.quote) {
            uint32_t j = 0;
            while (j < avail && s[j] == c) ++j;
            const uint32_t e = j + blankRun(s + j, avail - j);
            if (j >= st_.count && (e == avail || s[e] == '\n')) {
                st_.mode = M_NORMAL;
                return {Punct, j};
            }
        }
        return {Code, toEol(s, avail)};
    }
    if (st_.mode == M_MDCODE) {
        uint32_t j = 0;
        while (j < avail && s[j] != '\n') {
            if (s[j] == '`') {
                uint32_t r = 0;
                while (j + r < avail && s[j + r] == '`') ++r;
                if (r == st_.count) {
                    st_.mode = M_NORMAL;
                    return {Code, j + r};
                }
                j += r;
            } else {
                ++j;
            }
        }
        return {Code, j};
    }
    if (st_.mode == M_MDLINK) {
        switch (st_.linkPart) {
            case 1:  // link text
                st_.linkPart = 2;
                if (st_.partLen) return {Link, st_.partLen};
                [[fallthrough]];  // empty text
            case 2:  // "]("
                st_.linkPart = 3;
                return {Punct, 2};
            case 3:  // url
                st_.linkPart = 4;
                if (st_.partLen2) return {Comment, st_.partLen2};
                [[fallthrough]];  // empty url
            default:  // ")"
                st_.mode = M_NORMAL;
                st_.linkPart = 0;
                return {Punct, 1};
        }
    }

    if (bol) {
        if (isBlank(c)) {
            keepBol_ = true;
            return {Plain, blankRun(s, avail)};
        }
        // Headings
        if (c == '#') {
            uint32_t j = 0;
            while (j < avail && s[j] == '#') ++j;
            if (j <= 6 && (j == avail || isBlank(s[j]) || s[j] == '\n')) {
                st_.mode = M_LINE;
                st_.count = Heading;
                return {Heading, j};
            }
        }
        // Fences
        if (c == '`' || c == '~') {
            uint32_t j = 0;
            while (j < avail && s[j] == c) ++j;
            if (j >= 3) {
                st_.mode = M_FENCEOPEN;
                st_.quote = c;
                st_.count = uint8_t(j > 255 ? 255 : j);
                return {Punct, j};
            }
        }
        // Horizontal rules and setext underlines: a line of one marker char.
        if (c == '-' || c == '*' || c == '_' || c == '=') {
            const uint32_t e = toEol(s, avail);
            if (e < avail) {
                uint32_t marks = 0;
                bool ok = true;
                for (uint32_t j = 0; j < e && ok; ++j) {
                    if (s[j] == c) ++marks;
                    else if (!isBlank(s[j])) ok = false;
                }
                if (ok && (marks >= 3 || (c == '=' && marks >= 1))) return {Punct, e};
            }
        }
        // Block quotes: keep looking for block markers after '>'.
        if (c == '>') {
            keepBol_ = true;
            return {Punct, 1};
        }
        // List items, with an optional task checkbox.
        uint32_t m = 0;
        if ((c == '-' || c == '*' || c == '+') && avail > 1 && isBlank(s[1])) {
            m = 1;
        } else if (isDigit(c)) {
            uint32_t j = 0;
            while (j < avail && j < 9 && isDigit(s[j])) ++j;
            if (j + 1 < avail && (s[j] == '.' || s[j] == ')') && isBlank(s[j + 1])) m = j + 1;
        }
        if (m) {
            const uint32_t b = m + blankRun(s + m, avail - m);
            if (b + 3 <= avail && s[b] == '[' && (s[b + 1] == ' ' || (s[b + 1] | 0x20) == 'x') &&
                s[b + 2] == ']')
                return {Punct, b + 3};
            return {Punct, m};
        }
    }
    return mdInline(s, avail);
}

Lexer::Tok Lexer::mdInline(const uint8_t* s, uint32_t avail) {
    const uint8_t c = s[0];
    const uint8_t text = (st_.flags & (F_EM | F_STRONG)) ? Emph : Plain;
    const uint32_t eol = toEol(s, avail);

    if (c == '\\' && avail > 1 && s[1] != '\n') return {text, 2};
    if (c == '`') {
        uint32_t r = 0;
        while (r < avail && s[r] == '`') ++r;
        // Look for the closing run on this line; unmatched backticks are literal.
        // If the line continues past the lookahead, assume it closes.
        bool found = eol == avail && avail >= kLookahead;
        for (uint32_t j = r; j < eol && !found;) {
            if (s[j] == '`') {
                uint32_t k = 0;
                while (j + k < eol && s[j + k] == '`') ++k;
                if (k == r) found = true;
                j += k;
            } else {
                ++j;
            }
        }
        if (!found) return {text, r};
        st_.mode = M_MDCODE;
        st_.count = uint8_t(r > 255 ? 255 : r);
        return {Code, r};
    }
    if (c == '<') {
        if (startsWith(s, avail, "<!--")) { st_.mode = M_XCOMMENT; return {Comment, 4}; }
        if (startsWith(s, avail, "<http") || startsWith(s, avail, "<mailto:")) {
            for (uint32_t j = 1; j < eol; ++j)
                if (s[j] == '>') return {Link, j + 1};
        }
        return {text, 1};
    }
    if (c == '!' && avail > 1 && s[1] == '[') return {Punct, 1};
    if (c == '[') {
        // [text](url) entirely on this line and inside the lookahead.
        uint32_t j = 1;
        while (j < eol && s[j] != ']') ++j;
        if (j + 1 < eol && s[j + 1] == '(') {
            uint32_t k = j + 2;
            while (k < eol && s[k] != ')' && !isBlank(s[k])) ++k;
            if (k < eol && s[k] == ')' && j - 1 < 0x10000 && k - j - 2 < 0x10000) {
                st_.mode = M_MDLINK;
                st_.linkPart = 1;
                st_.partLen = uint16_t(j - 1);
                st_.partLen2 = uint16_t(k - j - 2);
                return {Punct, 1};
            }
        }
        return {text, 1};
    }
    if (c == '*' || c == '_') {
        uint32_t r = 0;
        while (r < avail && s[r] == c) ++r;
        const uint8_t before = st_.prev;
        const uint8_t after = r < avail ? s[r] : '\n';
        const bool beforeBlank = before == '\n' || isBlank(before);
        const bool afterBlank = after == '\n' || isBlank(after);
        bool canOpen = !afterBlank, canClose = !beforeBlank;
        if (c == '_') {  // no intra-word emphasis with underscores (snake_case)
            canOpen = canOpen && !isAlnum(before);
            canClose = canClose && !isAlnum(after);
        }
        const uint8_t flag = r >= 2 ? F_STRONG : F_EM;
        const uint32_t len = r >= 2 ? 2 : 1;
        if ((st_.flags & flag) && canClose) {
            st_.flags &= uint8_t(~flag);
            return {Punct, len};
        }
        if (!(st_.flags & flag) && canOpen) {
            st_.flags |= flag;
            return {Punct, len};
        }
        return {text, r};
    }
    if (c == '|') return {Punct, 1};
    if (isAlnum(c)) {
        // Words stop at '_' so that emphasis markers are seen (snake_case is
        // handled by the intra-word rule above).
        Tok w = {text, 0};
        while (w.len < avail && isAlnum(s[w.len])) ++w.len;
        if (!isAlnum(st_.prev) && ((w.len == 4 && startsWith(s, avail, "http://")) ||
                                   (w.len == 5 && startsWith(s, avail, "https://")))) {
            uint32_t j = w.len;
            while (j < eol && !isBlank(s[j]) && s[j] != '<' && s[j] != ')' && s[j] != ']' &&
                   s[j] != '>')
                ++j;
            if (j < eol || j < avail) {  // end known: trailing punctuation is not part of it
                while (j > w.len + 3 && strchr(".,;:!?", s[j - 1])) --j;
            }
            return {Link, j};
        }
        return {text, w.len};
    }
    // Run of other characters that cannot start anything.
    uint32_t j = 1;
    while (j < eol && !isAlnum(s[j]) && !strchr("\\`<![*_|", s[j])) ++j;
    return {text, j};
}

}  // namespace syn
