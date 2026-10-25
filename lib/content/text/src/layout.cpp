#include "pd/text/layout.h"

namespace pd::text {

uint32_t Layout::nextChar(uint32_t p) {
    const uint32_t size = tb_.size();
    if (p >= size) return size;
    const bool lead = tb_.at(p) >= 0xC0;
    ++p;
    if (lead)
        for (int k = 0; k < 3 && p < size && isCont(tb_.at(p)); ++k) ++p;
    return p;
}

uint32_t Layout::prevChar(uint32_t p) {
    if (p == 0) return 0;
    uint32_t q = p - 1;
    for (int k = 0; k < 3 && q > 0 && isCont(tb_.at(q)); ++k) --q;
    // Only treat it as one character if a real lead byte starts it.
    if (q < p - 1 && tb_.at(q) >= 0xC0) return q;
    return p - 1;
}

uint32_t Layout::lineStart(uint32_t pos) {
    uint32_t p = pos;
    const uint32_t limit = pos > kMaxBackScan ? pos - kMaxBackScan : 0;
    while (p > limit && tb_.at(p - 1) != '\n') --p;
    // A cut pseudo-line must not start inside a UTF-8 sequence.
    if (p == limit && p > 0 && tb_.at(p - 1) != '\n')
        for (int k = 0; k < 3 && p < pos && isCont(tb_.at(p)); ++k) ++p;
    return p;
}

uint32_t Layout::lineEnd(uint32_t pos) {
    const uint32_t size = tb_.size();
    uint32_t p = pos;
    while (p < size && tb_.at(p) != '\n') ++p;
    return p;
}

Row Layout::row(uint32_t start) {
    const uint32_t size = tb_.size();
    uint32_t p = start;
    int cells = 0;
    uint32_t lastBreak = 0;
    bool haveBreak = false;
    while (p < size) {
        const uint8_t c = tb_.at(p);
        if (c == '\n') return Row{start, p + 1, true};
        const int w = cellWidth(c);
        if (w && cells >= width_) {
            if (c == ' ') {  // let one space hang in the margin
                const uint32_t q = p + 1;
                if (q < size && tb_.at(q) == '\n') return Row{start, q + 1, true};
                return Row{start, q, false};
            }
            if (haveBreak) return Row{start, lastBreak, false};
            return Row{start, p, false};  // hard break inside a long word
        }
        if (c == ' ' || c == '\t') {
            lastBreak = p + 1;
            haveBreak = true;
        }
        cells += w;
        ++p;
    }
    return Row{start, size, false};
}

Row Layout::rowContaining(uint32_t pos) {
    const uint32_t size = tb_.size();
    uint32_t s = lineStart(pos);
    for (;;) {
        const Row r = row(s);
        if (pos < r.next || r.next == s || (r.next == size && !r.nl)) return r;
        s = r.next;
    }
}

uint32_t Layout::prevRowStart(uint32_t start) {
    if (start == 0) return 0;
    uint32_t s = lineStart(start - 1);
    for (;;) {
        const Row r = row(s);
        if (r.next >= start || r.next == s) return s;
        s = r.next;
    }
}

uint32_t Layout::rowLastPos(const Row& r) {
    if (r.nl) return r.next - 1;
    if (r.next >= tb_.size()) return tb_.size();
    return prevChar(r.next);
}

int Layout::colOf(const Row& r, uint32_t pos) {
    int cells = 0;
    for (uint32_t p = r.start; p < pos; ++p) cells += cellWidth(tb_.at(p));
    return cells;
}

uint32_t Layout::posAtCol(const Row& r, int col) {
    const uint32_t last = rowLastPos(r);
    uint32_t p = r.start;
    int cells = 0;
    while (p < last && cells < col) {
        cells += cellWidth(tb_.at(p));
        p = nextChar(p);
    }
    return p > last ? last : p;
}

}  // namespace pd::text
