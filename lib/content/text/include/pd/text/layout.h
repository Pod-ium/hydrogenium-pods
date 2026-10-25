// Soft-wrap layout over a TextBuffer.
//
// Visual rows are computed on demand from the start of a logical line, so
// nothing proportional to the file size is kept in RAM. UTF-8 continuation
// bytes and '\r' are zero-width; every other byte starts one cell.
#pragma once
#include <cstdint>

#include "pd/text/text_buffer.h"

namespace pd::text {

struct Row {
    uint32_t start;  // first byte of the row
    uint32_t next;   // first byte of the following row
    bool nl;         // row ends with '\n' (included in [start, next))
};

class Layout {
public:
    // Lines longer than this are cut into pseudo-lines when scanning back.
    static constexpr uint32_t kMaxBackScan = 64 * 1024;

    explicit Layout(TextBuffer& tb) : tb_(tb) {}
    void setWidth(int cells) { width_ = cells < 4 ? 4 : cells; }
    int width() const { return width_; }

    static bool isCont(uint8_t c) { return (c & 0xC0) == 0x80; }
    static int cellWidth(uint8_t c) { return (isCont(c) || c == '\r') ? 0 : 1; }

    uint32_t nextChar(uint32_t p);
    uint32_t prevChar(uint32_t p);
    uint32_t lineStart(uint32_t pos);  // start of logical line containing pos
    uint32_t lineEnd(uint32_t pos);    // position of '\n' (or size)

    Row row(uint32_t start);
    Row rowContaining(uint32_t pos);
    // Start of the visual row before the row starting at `start` (start > 0).
    uint32_t prevRowStart(uint32_t start);
    // Last position the cursor may occupy inside a row.
    uint32_t rowLastPos(const Row& r);
    int colOf(const Row& r, uint32_t pos);
    uint32_t posAtCol(const Row& r, int col);

private:
    TextBuffer& tb_;
    int width_ = 38;
};

}  // namespace pd::text
