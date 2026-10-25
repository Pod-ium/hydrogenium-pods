// PicoPods - Git client: clone over HTTPS (smart HTTP protocol, shallow by
// default) straight onto the SD card.
//
// The result is a normal Git repository: the working files plus a .git
// folder with the pack, its index (.idx), refs, config and a Git index, so
// `git status`, `git log` or `git pull` work on a computer with the card.
//
// RAM stays bounded: the pack is streamed to the card, large objects and
// delta chains are resolved through temporary files, and only a table of
// object IDs (28 bytes per object) is kept in memory.
//
//   pd::git::CloneOptions o;
//   o.url = "https://github.com/user/repo";
//   o.parent_dir = "/code";
//   pd::git::CloneResult r = pd::git::clone(o, hooks);   // -> /code/repo
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace pd::git {

enum class Phase {
    Connecting,    // talking to the server
    Receiving,     // downloading the pack (done/total: bytes; total 0 = unknown)
    Indexing,      // reading objects (done/total: objects)
    Resolving,     // rebuilding delta-compressed objects (objects)
    CheckingOut,   // writing files (files written; total 0 = unknown)
};
const char* phase_name(Phase p);

struct CloneOptions {
    std::string url;          // https://host/owner/repo[.git]; "owner/repo" means GitHub
    std::string parent_dir;   // the repository folder is created inside it
    std::string dir_name;     // default: the repository name from the URL
    std::string branch;       // default: the server's default branch
    int depth = 1;            // commits of history; 0 = all of it
};

struct CloneHooks {
    // Return false to cancel (the partial folder is removed).
    std::function<bool(Phase phase, uint64_t done, uint64_t total, const std::string& detail)> progress;
};

struct CloneResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string dir;        // repository folder
    std::string branch;     // checked-out branch
    std::string commit;     // its commit ID (hex)
    uint32_t files = 0;     // files written
    uint32_t skipped = 0;   // entries the card cannot store (names FAT refuses, submodules)
    uint32_t objects = 0;
    uint64_t pack_bytes = 0;
};

// "https://github.com/a/b.git", "github.com/a/b", "a/b" -> "https://github.com/a/b.git".
// Empty if it does not look like a repository URL.
std::string normalize_url(const std::string& url);
// "https://host/a/b.git" -> "b".
std::string repo_name(const std::string& url);

CloneResult clone(const CloneOptions& opt, const CloneHooks& hooks);

// Limits (the device has little RAM). Set before clone(); the defaults suit
// a Pico 2 W.
struct Limits {
    uint32_t max_objects;       // objects in the pack
    uint32_t mem_object_bytes;  // larger objects go through temporary files
    uint32_t cache_bytes;       // RAM cache of rebuilt objects (delta bases)
};
Limits& limits();

}  // namespace pd::git
