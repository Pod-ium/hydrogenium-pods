// Randomized test of TextBuffer + Layout against a std::string model.
// Build & run:  ctest (PicoPods host build)
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "pd/text/layout.h"
#include "pd/text/text_buffer.h"

using namespace pd::text;

static std::mt19937 rng(12345);
static int rnd(int n) { return n <= 0 ? 0 : int(rng() % unsigned(n)); }

static std::string contents(TextBuffer& tb) {
    std::string s(tb.size(), '\0');
    uint32_t got = tb.read(0, reinterpret_cast<uint8_t*>(&s[0]), tb.size());
    if (got != tb.size()) { printf("short read %u/%u\n", got, tb.size()); exit(1); }
    // cross-check at()
    for (int k = 0; k < 20 && tb.size(); ++k) {
        uint32_t p = rnd(tb.size());
        if (tb.at(p) != uint8_t(s[p])) { printf("at() mismatch\n"); exit(1); }
    }
    return s;
}

static void check(TextBuffer& tb, const std::string& ref, const char* what) {
    std::string got = contents(tb);
    if (got != ref) {
        printf("MISMATCH after %s: size %zu vs %zu\n", what, got.size(), ref.size());
        for (size_t i = 0; i < std::min(got.size(), ref.size()); ++i)
            if (got[i] != ref[i]) { printf(" first diff at %zu\n", i); break; }
        exit(1);
    }
}

static std::string randText(int maxLen) {
    static const char* frags[] = {"a", "b", "word", " ", " ", "\n", "è", "à", "ù", "hello ",
                                  "\r\n", "x", "lorem ipsum ", "\t"};
    std::string s;
    int n = 1 + rnd(maxLen);
    while (int(s.size()) < n) s += frags[rnd(14)];
    return s;
}

static void checkLayout(TextBuffer& tb, Layout& lay) {
    const uint32_t size = tb.size();
    uint32_t s = 0;
    std::vector<Row> rows;
    for (int guard = 0;; ++guard) {
        Row r = lay.row(s);
        rows.push_back(r);
        if (r.next == s) break;          // empty EOF row
        if (r.next == size && !r.nl) break;
        if (r.next <= s) { printf("layout no progress\n"); exit(1); }
        s = r.next;
        if (guard > 1000000) { printf("layout loop\n"); exit(1); }
    }
    // consecutive rows: prevRowStart must invert row()
    for (size_t i = 1; i < rows.size(); ++i) {
        uint32_t p = lay.prevRowStart(rows[i].start);
        if (p != rows[i - 1].start) {
            printf("prevRowStart mismatch at row %zu: %u vs %u\n", i, p, rows[i - 1].start);
            auto pr = [&](Row r) { printf("[%u,%u) nl=%d '", r.start, r.next, r.nl);
                for (uint32_t q = r.start; q < r.next; ++q) { uint8_t c = tb.at(q);
                    if (c == '\n') printf("\\n"); else if (c == '\r') printf("\\r"); else if (c == '\t') printf("\\t"); else putchar(c); }
                printf("'\n"); };
            for (size_t k = (i > 3 ? i - 3 : 0); k <= i; ++k) pr(rows[k]);
            uint32_t L = lay.lineStart(rows[i].start - 1);
            printf("lineStart=%u\n", L);
            uint32_t q = L; for (int k = 0; k < 8; ++k) { Row r = lay.row(q); pr(r); if (r.next >= rows[i].start) break; q = r.next; }
            exit(1);
        }
    }
    for (int k = 0; k < 30; ++k) {
        uint32_t pos = rnd(size + 1);
        Row r = lay.rowContaining(pos);
        bool ok = (r.start <= pos) && (pos < r.next || (r.next == size && !r.nl) || r.next == r.start);
        if (!ok) { printf("rowContaining bad for %u: [%u,%u)\n", pos, r.start, r.next); exit(1); }
        uint32_t back = lay.posAtCol(r, lay.colOf(r, pos));
        // Positions of zero-width bytes may snap; must stay within the row.
        if (back < r.start || back > lay.rowLastPos(r)) { printf("posAtCol out of row\n"); exit(1); }
    }
}

int main() {
    (void)!system("rm -rf /tmp/fwtest && mkdir -p /tmp/fwtest/work");
    // Initial file bigger than the page cache + tail.
    std::string ref;
    while (ref.size() < 60000) ref += randText(40);
    {
        FILE* f = fopen("/tmp/fwtest/doc.txt", "wb");
        fwrite(ref.data(), 1, ref.size(), f);
        fclose(f);
    }
    TextBuffer tb;
    if (!tb.init("/tmp/fwtest/work")) { printf("init failed\n"); return 1; }
    if (!tb.load("/tmp/fwtest/doc.txt")) { printf("load failed\n"); return 1; }
    check(tb, ref, "load");
    Layout lay(tb);
    lay.setWidth(20);
    checkLayout(tb, lay);

    std::vector<std::string> history{ref};
    for (int round = 0; round < 6; ++round) {
        for (int op = 0; op < 3000; ++op) {
            int kind = rnd(10);
            if (kind < 5) {  // insert (typing-like: often at the last pos)
                std::string t = randText(kind == 0 ? 300 : 3);
                uint32_t pos = rnd(ref.size() + 1);
                if (!tb.insert(pos, t.data(), t.size())) { printf("insert: %s\n", tb.lastError().c_str()); return 1; }
                ref.insert(pos, t);
            } else if (kind < 8) {
                if (ref.empty()) continue;
                uint32_t pos = rnd(ref.size());
                uint32_t n = 1 + rnd(kind == 5 ? 2000 : 5);
                if (!tb.erase(pos, n)) { printf("erase: %s\n", tb.lastError().c_str()); return 1; }
                ref.erase(pos, n);
            } else if (kind == 8) {  // clipboard copy + paste
                if (ref.empty()) continue;
                uint32_t a = rnd(ref.size());
                uint32_t n = 1 + rnd(std::min<size_t>(9000, ref.size() - a));
                if (!tb.copyToFile(a, n, "/tmp/fwtest/work/clip")) return 1;
                uint32_t pos = rnd(ref.size() + 1), len = 0;
                std::string piece = ref.substr(a, n);
                if (!tb.insertFile(pos, "/tmp/fwtest/work/clip", &len)) return 1;
                if (len != n) { printf("paste len\n"); return 1; }
                ref.insert(pos, piece);
            } else {  // search
                if (ref.size() < 10) continue;
                uint32_t a = rnd(ref.size() - 5);
                std::string needle = ref.substr(a, 1 + rnd(5));
                uint32_t from = rnd(ref.size());
                uint32_t got = tb.find(needle, from, true, true);
                size_t exp = ref.find(needle, from);
                if (exp == std::string::npos) exp = ref.find(needle);
                if (got != exp) { printf("find mismatch %u vs %zu\n", got, exp); return 1; }
            }
            tb.breakUndoMerge();
            if (op % 500 == 0) check(tb, ref, "ops");
        }
        check(tb, ref, "round");
        checkLayout(tb, lay);

        // Undo a few steps, verify redo returns through the same states.
        std::vector<std::string> states{ref};
        uint32_t cur;
        int undos = 0;
        while (undos < 50 && tb.undo(&cur)) {
            states.push_back(contents(tb));
            ++undos;
        }
        for (int k = undos - 1; k >= 0; --k) {
            if (!tb.redo(&cur)) { printf("redo failed\n"); return 1; }
            check(tb, states[k], "redo");
        }
        check(tb, ref, "undo/redo");

        // Grouped edits undo as one step.
        tb.beginGroup();
        tb.insert(0, "AAA", 3);
        tb.erase(10, 5);
        tb.insert(20, "BBB", 3);
        tb.endGroup();
        tb.undo(&cur);
        check(tb, ref, "group undo");
        tb.redo(&cur);
        std::string g = ref;
        g.insert(0, "AAA"); g.erase(10, 5); g.insert(20, "BBB");
        check(tb, g, "group redo");
        ref = g;

        printf("round %d: size=%zu pieces=%zu\n", round, ref.size(), tb.pieceCount());
        // Save over itself, reload, compare.
        if (round % 2 == 0) {
            if (!tb.saveTo("/tmp/fwtest/doc.txt")) { printf("save: %s\n", tb.lastError().c_str()); return 1; }
        } else {
            if (!tb.saveTo("/tmp/fwtest/copy.txt")) { printf("save as: %s\n", tb.lastError().c_str()); return 1; }
        }
        if (tb.pieceCount() > 1) { printf("not compacted\n"); return 1; }
        check(tb, ref, "save");
    }

    // Undo merges typing: type "hello world" char by char -> 2 undo steps.
    tb.newDocument();
    const char* hw = "hello world";
    for (int i = 0; hw[i]; ++i) tb.insert(i, hw + i, 1);
    uint32_t cur;
    tb.undo(&cur);
    check(tb, "hello ", "word undo");
    tb.undo(&cur);
    check(tb, "", "word undo 2");
    printf("ALL TESTS PASSED\n");
    return 0;
}
