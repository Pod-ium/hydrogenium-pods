// Host tests for the syntax highlighter (lib/content/text/src/syntax.cpp).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pd/text/syntax.h"

using namespace pd::syn;

static const char kLetters[] = ".ktsncmTapheCl";  // one per syn::Cls

// Lexes `text` in one go and returns one letter per byte ('\n' kept).
static std::string classes(Lang lang, const std::string& text) {
    Lexer lx;
    lx.reset(lang);
    std::vector<uint8_t> out(text.size());
    const uint8_t* s = reinterpret_cast<const uint8_t*>(text.data());
    lx.run(s, uint32_t(text.size()), uint32_t(text.size()), out.data());
    std::string r;
    for (size_t i = 0; i < text.size(); ++i) r += text[i] == '\n' ? '\n' : kLetters[out[i]];
    return r;
}

// Same, but in random chunks with only kLookahead bytes of lookahead, the way
// the UI feeds rows; also snapshots/restores the state between chunks.
static std::string classesChunked(Lang lang, const std::string& text, unsigned seed) {
    srand(seed);
    Lexer lx;
    lx.reset(lang);
    std::vector<uint8_t> out(text.size());
    const uint8_t* s = reinterpret_cast<const uint8_t*>(text.data());
    uint32_t pos = 0;
    while (pos < text.size()) {
        uint32_t n = 1 + uint32_t(rand() % 40);
        if (pos + n > text.size()) n = uint32_t(text.size()) - pos;
        uint32_t avail = n + kLookahead;
        if (pos + avail > text.size()) avail = uint32_t(text.size()) - pos;
        lx.run(s + pos, n, avail, out.data() + pos);
        const LexState snap = lx.state();
        Lexer other;
        other.setState(snap);
        lx = other;
        pos += n;
    }
    std::string r;
    for (size_t i = 0; i < text.size(); ++i) r += text[i] == '\n' ? '\n' : kLetters[out[i]];
    return r;
}

static int failures = 0;

static void check(Lang lang, const std::string& text, const std::string& want) {
    const std::string got = classes(lang, text);
    if (got != want) {
        ++failures;
        printf("FAIL (%s)\n  text: %s\n  want: %s\n  got : %s\n", langName(lang), text.c_str(),
               want.c_str(), got.c_str());
    }
    for (unsigned seed = 1; seed <= 25; ++seed) {
        const std::string c = classesChunked(lang, text, seed);
        if (c != got) {
            ++failures;
            printf("FAIL chunked (%s, seed %u)\n  text: %s\n  whole: %s\n  chunk: %s\n",
                   langName(lang), seed, text.c_str(), got.c_str(), c.c_str());
            break;
        }
    }
}

int main() {
    // language detection
    assert(langForPath("/a/b/README.md") == Markdown);
    assert(langForPath("/robot.URDF") == Xml);
    assert(langForPath("/x/app.cpp") == Cpp);
    assert(langForPath("/x/lcd.h") == Cpp);
    assert(langForPath("/x/tool.py") == Python);
    assert(langForPath("/notes.txt") == None);
    assert(langForPath("/dir.md/file") == None);
    assert(langForPath("") == None);

    // ---- C++ --------------------------------------------------------------
    check(Cpp, "int x = 42;", "ttt.....nn.");
    check(Cpp, "#include <cstdio>", "mmmmmmmm.ssssssss");
    check(Cpp, "return \"a\\\"b\"; // hi", "kkkkkk.ssssss..ccccc");
    check(Cpp, "/* a\nb */ if", "cccc\ncccc.kk");
    check(Cpp, "auto s = R\"x(a)\"\n)x\";", "kkkk.....sssssss\nsss.");
    check(Cpp, "x.for_each(0x1Fu, 1e-3f);", "...........nnnnn..nnnnn..");
    check(Cpp, "char c = '\\'';", "tttt.....ssss.");
    check(Cpp, "#define A \\\n  B\nint", "mmmmmmm....\n...\nttt");
    check(Cpp, "u8\"hi\" nullptr", "ssssss.kkkkkkk");

    // ---- Python -----------------------------------------------------------
    check(Python, "def foo(self):", "kkk.TTT.tttt..");
    check(Python, "x = f'{a}' # c", "....ssssss.ccc");
    check(Python, "s = \"\"\"a\nb\"\"\" + 1", "....ssss\nssss...n");
    check(Python, "@dataclass\nclass A:", "mmmmmmmmmm\nkkkkk.T.");
    check(Python, "m = re.match(x)", "..............." );
    check(Python, "if x is None:", "kk...kk.kkkk.");

    // ---- XML --------------------------------------------------------------
    check(Xml, "<?xml version=\"1.0\"?>", "mmmmmmmmmmmmmmmmmmmmm");
    check(Xml, "<link name=\"base\"/>", "pTTTT.aaaapsssssspp");
    check(Xml, "<a\n  x='1'>t&amp;</a>", "pT\n..apsssp.mmmmmppTp");
    check(Xml, "<!-- c\nc --><b>", "cccccc\ncccccpTp");
    check(Xml, "<![CDATA[<x>]]>", "mmmmmmmmmssssss");

    // ---- Markdown ---------------------------------------------------------
    check(Markdown, "# Title\ntext", "hhhhhhh\n....");
    check(Markdown, "- [ ] task", "ppppp.....");
    check(Markdown, "1. **bold** and _em_", "pp.ppeeeepp.....peep");
    check(Markdown, "a snake_case_name", ".................");
    check(Markdown, "use `x = 1` now", "....CCCCCCC....");
    check(Markdown, "see [doc](http://a.b) ok", "....plllppccccccccccp...");
    check(Markdown, "```py\ncode # x\n```\nafter", "pppmm\nCCCCCCCC\nppp\n.....");
    check(Markdown, "---\n> quote", "ppp\np......");
    check(Markdown, "2 * 3 * 4", ".........");
    check(Markdown, "go to https://x.y/z now", "......lllllllllllll....");
    check(Markdown, "a `b", "....");
    check(Markdown, "see https://iit.it.\nx", "....llllllllllllll.\n.");

    // keyword tables: first / last entries (the lookup is a binary search)
    check(Cpp, "NULL while bool wchar_t", "kkkk.kkkkk.tttt.ttttttt");
    check(Python, "False yield AttributeError zip", "kkkkk.kkkkk.tttttttttttttt.ttt");

    // fuzz: random snippets of syntax-heavy characters must not crash and
    // must lex the same in chunks as in one piece (lines kept short)
    const char alphabet[] = "ab_19 \t\n\"'`#<>/*![]()-=&;:.@\\?x";
    int diffs = 0;
    for (int l = 0; l < kLangCount; ++l) {
        for (unsigned it = 0; it < 3000; ++it) {
            srand(it * 7919 + l);
            std::string txt;
            const int len = rand() % 120;
            int col = 0;
            for (int i = 0; i < len; ++i) {
                char c = alphabet[rand() % (sizeof alphabet - 1)];
                if (++col > 30) c = '\n';
                if (c == '\n') col = 0;
                txt += c;
            }
            const std::string whole = classes(Lang(l), txt);
            if (classesChunked(Lang(l), txt, it) != whole) ++diffs;
        }
    }
    if (diffs) {
        printf("FAIL: %d fuzz cases lex differently in chunks\n", diffs);
        ++failures;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("syntax tests passed\n");
    return 0;
}
