// Piece-table text buffer backed by the SD card.
//
// The document is a sequence of "pieces", each pointing into one of two
// byte sources that live on the SD card:
//   ORIG - the file as it was loaded (opened read-only, never modified)
//   ADD  - an append-only scratch file that receives every typed/pasted byte
// RAM only holds the piece list, the undo history (which is itself just
// piece lists), a small page cache and a 4 KB tail of the ADD file.
// Opening a 1 GB file therefore costs one 8-byte piece.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pd/fs.h"

namespace pd::text {

class TextBuffer {
public:
    // Bit 31 of Piece::start selects the source.
    static constexpr uint32_t kAddFlag = 0x80000000u;
    struct Piece {
        uint32_t start;  // offset in source | kAddFlag for ADD
        uint32_t len;
    };

    // Returns false to abort a long operation. Argument: done / total.
    using Progress = std::function<bool(uint32_t done, uint32_t total)>;

    // Configuration
    static constexpr int kPageSize = 512;
    static constexpr int kPageCount = 32;            // 16 KB page cache
    static constexpr uint32_t kTailSize = 4096;      // RAM tail of ADD file
    static constexpr size_t kMaxPieces = 12000;      // document fragmentation limit
    static constexpr size_t kMaxUndoPieces = 8000;   // pieces kept in history
    static constexpr size_t kMaxUndoRecords = 400;

    TextBuffer();
    ~TextBuffer();

    // workDir must exist; the ADD file lives there.
    bool init(const std::string& workDir);

    // ---- document lifecycle -------------------------------------------
    bool newDocument();
    bool load(const std::string& path);
    // Writes the whole document to `path` (via a temp file + rename), then
    // reloads from it (this clears the undo history).
    bool saveTo(const std::string& path, const Progress& progress = nullptr);
    const std::string& lastError() const { return err_; }
    // True if the last saveTo() wrote the file but could not reopen it; the
    // buffer is then an empty untitled document.
    bool reopenFailed() const { return reopenFailed_; }

    // ---- reading --------------------------------------------------------
    uint32_t size() const { return size_; }
    uint8_t at(uint32_t pos);  // pos < size()
    // Bulk read, returns bytes read.
    uint32_t read(uint32_t pos, uint8_t* buf, uint32_t n);

    // ---- editing (all edits are undoable) -------------------------------
    bool insert(uint32_t pos, const char* s, uint32_t n);
    bool erase(uint32_t pos, uint32_t n);
    // Streams [pos, pos+n) into a file (used for the clipboard).
    bool copyToFile(uint32_t pos, uint32_t n, const std::string& path);
    // Inserts the content of a file at pos; *outLen receives its length.
    bool insertFile(uint32_t pos, const std::string& path, uint32_t* outLen);

    // ---- undo -----------------------------------------------------------
    // Consecutive typing / deleting is merged into one step until this is
    // called (the UI calls it on cursor movement).
    void breakUndoMerge() { canMerge_ = false; }
    // Edits between beginGroup/endGroup undo as one step (replace-all).
    void beginGroup();
    void endGroup();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // On success *cursor is set to where the change happened.
    bool undo(uint32_t* cursor);
    bool redo(uint32_t* cursor);

    // ---- state ------------------------------------------------------------
    bool modified() const { return modified_; }
    void setModified(bool m) { modified_ = m; }
    const std::string& path() const { return path_; }
    size_t pieceCount() const { return pieces_.size(); }
    // True when the piece list is near its limit: the UI should suggest saving
    // (saving compacts the document back to a single piece).
    bool nearlyFull() const { return pieces_.size() > kMaxPieces * 9 / 10; }
    // Lowest document position changed since the previous call (UINT32_MAX if
    // nothing changed), then resets. Loading or clearing the document reports 0.
    // Used by the syntax highlighter to keep its cached state when possible.
    uint32_t takeDirtyFrom() {
        const uint32_t d = dirtyFrom_;
        dirtyFrom_ = UINT32_MAX;
        return d;
    }

    // Used by search: case-insensitive (ASCII) search. Returns found pos or
    // UINT32_MAX. Searches [from, size) then wraps to [0, from) if wrap.
    uint32_t find(const std::string& needle, uint32_t from, bool wrap,
                  bool matchCase, const Progress& progress = nullptr);

private:
    struct UndoRec {
        uint8_t kind;  // 0 = insert, 1 = delete
        uint32_t pos;
        uint32_t len;
        uint32_t group;
        std::vector<Piece> pcs;
    };
    struct Page {
        uint32_t base = 0;
        uint32_t lru = 0;
        uint16_t valid = 0;
        uint8_t src = 0xFF;  // 0 orig, 1 add, 0xFF empty
        uint8_t data[kPageSize];
    };

    // piece-table primitives
    void locate(uint32_t pos, size_t* idx, uint32_t* pieceStart);
    void insertPieces(uint32_t pos, const std::vector<Piece>& pcs);
    void removeRange(uint32_t pos, uint32_t n, std::vector<Piece>* removed);
    void mergeAround(size_t i);
    static bool contiguous(const Piece& a, const Piece& b) {
        return ((a.start & kAddFlag) == (b.start & kAddFlag)) &&
               (a.start + a.len == b.start);
    }
    static void appendPiece(std::vector<Piece>& v, const Piece& p);

    // sources
    bool appendAdd(const uint8_t* data, uint32_t n);
    bool flushTail();
    uint32_t addLen() const { return addFlushed_ + tailLen_; }
    uint32_t readSource(uint32_t start, uint8_t* buf, uint32_t n);  // start has flag
    const uint8_t* page(uint8_t src, uint32_t off, uint32_t* avail);
    void invalidatePages(uint8_t src);
    bool resetAdd();

    // undo helpers
    void pushUndo(uint8_t kind, uint32_t pos, std::vector<Piece>&& pcs);
    void trimUndo();
    size_t undoPieceTotal() const;
    bool apply(const UndoRec& r, bool inverse, uint32_t* cursor);

    std::string workDir_, addPath_, path_, err_;
    bool reopenFailed_ = false;
    fs::File orig_, add_;
    uint32_t origSize_ = 0;
    uint32_t addFlushed_ = 0;
    uint8_t* tail_ = nullptr;
    uint32_t tailLen_ = 0;

    std::vector<Piece> pieces_;
    uint32_t size_ = 0;
    size_t cacheIdx_ = 0;       // locate() cache
    uint32_t cacheStart_ = 0;

    Page* pages_ = nullptr;
    uint32_t lruClock_ = 0;
    int lastPage_ = -1;

    std::vector<UndoRec> undo_, redo_;
    bool canMerge_ = false;
    uint32_t groupCounter_ = 1;
    uint32_t activeGroup_ = 0;
    int groupDepth_ = 0;
    bool modified_ = false;
    uint32_t dirtyFrom_ = 0;
};

}  // namespace pd::text
