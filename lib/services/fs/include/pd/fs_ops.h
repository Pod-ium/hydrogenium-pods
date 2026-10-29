// PicoPods - operations on whole trees: copy, move and delete files and
// folders, with progress, cancellation and name conflicts.
//
// Built on pd::fs only, so it behaves the same on the SD card and in host
// tests. Hidden entries are always included: copying a folder copies all of
// it, deleting a folder deletes all of it.
//
//   pd::fs::OpHooks h;
//   h.progress = [&](uint64_t done, uint64_t total, const std::string& cur) { ...; return !cancel; };
//   h.conflict = [&](const std::string& target, bool src_dir, bool dst_dir) { return pd::fs::Conflict::Skip; };
//   pd::fs::OpResult r = pd::fs::copy({"/a.txt", "/photos"}, "/backup", h);
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pd/fs.h"

namespace pd::fs {

// ---------------------------------------------------------------- names
// A name FAT accepts for a file or folder (no / \ : * ? " < > |, no control
// characters, not "." or "..", no trailing dot or space, <= 255 bytes).
bool valid_name(const std::string& name);
// True if `path` is `dir` or lies inside it. ASCII case is ignored (FAT).
bool same_or_inside(const std::string& path, const std::string& dir);
bool same_path(const std::string& a, const std::string& b);
// A name not used in `dir`: "report.txt" -> "report (2).txt" -> "report (3).txt".
// Folders keep dots in their name: "v1.2" -> "v1.2 (2)".
std::string unique_name(const std::string& dir, const std::string& name, bool is_dir = false);

// ---------------------------------------------------------------- sizes
struct TreeStats {
    uint32_t files = 0;
    uint32_t dirs = 0;     // folders, including the scanned one if it is a folder
    uint64_t bytes = 0;
};
// Adds the files, folders and bytes under `path` (a file or a folder) to
// `out`. keep_going (optional) is called between entries; false stops.
bool scan(const std::string& path, TreeStats& out, const std::function<bool()>& keep_going = nullptr);

// ---------------------------------------------------------------- operations
enum class Conflict {
    Replace,    // delete what is there, then copy / move
    Skip,       // leave both alone
    KeepBoth,   // copy / move under a new name ("name (2).ext")
    Merge,      // folder onto folder: go inside and resolve entry by entry
    Cancel,     // stop the whole operation
};

struct OpHooks {
    // Called while the operation runs (at most every progress_ms, and at the end).
    // done/total are work units (bytes plus a fixed cost per entry). Return
    // false to cancel: a partly copied file is removed.
    std::function<bool(uint64_t done, uint64_t total, const std::string& current)> progress;
    // `target` exists. src_dir/dst_dir say which sides are folders. Merge is
    // only valid when both are. nullptr: Skip.
    std::function<Conflict(const std::string& target, bool src_dir, bool dst_dir)> conflict;
    uint32_t buffer_bytes = 32 * 1024;   // copy buffer (a multiple of 512 is fastest)
    uint32_t progress_ms = 100;          // minimum time between progress calls
};

struct OpResult {
    uint32_t done = 0;      // entries copied / moved / deleted
    uint32_t skipped = 0;   // entries left alone after a conflict
    uint32_t failed = 0;    // entries that could not be processed
    bool cancelled = false;
    std::string error;      // first failure: "name: reason"
    bool ok() const { return failed == 0 && !cancelled; }
};

// Copies each source (file or folder, absolute path) into the folder dst_dir.
// A source pasted into its own folder is copied as "name (2)".
OpResult copy(const std::vector<std::string>& sources, const std::string& dst_dir, const OpHooks& hooks);
// Moves each source into dst_dir: a rename when possible (instant), otherwise
// copy then delete. The source is deleted only if its copy is complete.
OpResult move(const std::vector<std::string>& sources, const std::string& dst_dir, const OpHooks& hooks);
// Deletes files and whole folders (read-only entries too). Refuses "/".
OpResult remove_all(const std::vector<std::string>& paths, const OpHooks& hooks);

}  // namespace pd::fs
