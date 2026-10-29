// Host tests for pd/fs_ops.h: copy, move and delete of files and folder
// trees, name conflicts, progress and cancellation. Runs on a scratch folder
// (argv[1]) used as the card root.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pd/fs.h"
#include "pd/fs_ops.h"
#include "pd/path.h"

using namespace pd;

static int g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void put(const char* p, const std::string& text) { CHECK(fs::write_file(p, text, false)); }
static std::string get(const char* p) {
    std::string s;
    fs::read_file(p, s);
    return s;
}

static void wipe() {
    std::vector<fs::DirEntry> all;
    fs::list_dir("/", all, 1000, true);
    std::vector<std::string> paths;
    for (const auto& e : all) paths.push_back("/" + e.name);
    fs::remove_all(paths, fs::OpHooks());
    all.clear();
    fs::list_dir("/", all, 1000, true);
    CHECK(all.empty());
}

// /src/a.txt, /src/.hidden, /src/sub/b.bin (100 KB), /src/sub/empty/
static void make_tree() {
    CHECK(fs::mkdirs("/src/sub/empty"));
    put("/src/a.txt", "alpha");
    put("/src/.hidden", "h");
    std::string big(100 * 1024, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + i % 26);
    put("/src/sub/b.bin", big);
}

static void test_names() {
    fprintf(stderr, "[fs_ops] names\n");
    CHECK(fs::valid_name("notes.md") && fs::valid_name("Ettore's file (2)"));
    CHECK(!fs::valid_name("") && !fs::valid_name(".") && !fs::valid_name(".."));
    CHECK(!fs::valid_name("a/b") && !fs::valid_name("a:b") && !fs::valid_name("what?"));
    CHECK(!fs::valid_name("trailing.") && !fs::valid_name("trailing "));
    CHECK(fs::same_or_inside("/a/b", "/a") && fs::same_or_inside("/A/b", "/a") && fs::same_or_inside("/a", "/a"));
    CHECK(!fs::same_or_inside("/ab", "/a") && !fs::same_or_inside("/a", "/a/b"));
    CHECK(fs::same_or_inside("/anything", "/"));
    CHECK(fs::same_path("/Dir/File.TXT", "/dir/file.txt") && !fs::same_path("/a", "/b"));

    wipe();
    put("/report.txt", "1");
    CHECK(fs::unique_name("/", "report.txt") == "report (2).txt");
    put("/report (2).txt", "2");
    CHECK(fs::unique_name("/", "report.txt") == "report (3).txt");
    CHECK(fs::unique_name("/", "report (2).txt") == "report (3).txt");
    CHECK(fs::unique_name("/", "fresh.txt") == "fresh.txt");
    CHECK(fs::mkdir("/v1.2"));
    CHECK(fs::unique_name("/", "v1.2", true) == "v1.2 (2)");
}

static void test_scan_copy() {
    fprintf(stderr, "[fs_ops] scan + copy\n");
    wipe();
    make_tree();
    fs::TreeStats st;
    CHECK(fs::scan("/src", st));
    CHECK(st.files == 3 && st.dirs == 3 && st.bytes == 5 + 1 + 100 * 1024);

    CHECK(fs::mkdir("/dst"));
    uint64_t last_done = 0, last_total = 0;
    int calls = 0;
    fs::OpHooks h;
    h.buffer_bytes = 4096;
    h.progress = [&](uint64_t d, uint64_t t, const std::string&) {
        CHECK(d >= last_done && d <= t);
        last_done = d;
        last_total = t;
        ++calls;
        return true;
    };
    fs::OpResult r = fs::copy({"/src"}, "/dst", h);
    CHECK(r.ok() && r.done > 0 && r.skipped == 0);
    CHECK(calls >= 2 && last_done == last_total);
    CHECK(get("/dst/src/a.txt") == "alpha");
    CHECK(get("/dst/src/.hidden") == "h");
    CHECK(get("/dst/src/sub/b.bin") == get("/src/sub/b.bin"));
    CHECK(fs::is_dir("/dst/src/sub/empty"));
    CHECK(fs::is_dir("/src/sub/empty"));   // source untouched

    // Pasting into the same folder makes a copy with a new name.
    r = fs::copy({"/src/a.txt"}, "/src", fs::OpHooks());
    CHECK(r.ok() && get("/src/a (2).txt") == "alpha");

    // A folder cannot be copied into itself.
    r = fs::copy({"/src"}, "/src/sub", fs::OpHooks());
    CHECK(r.failed == 1 && !fs::exists("/src/sub/src"));

    // Missing destination folder.
    r = fs::copy({"/src/a.txt"}, "/nowhere", fs::OpHooks());
    CHECK(r.failed == 1);
}

static void test_conflicts() {
    fprintf(stderr, "[fs_ops] conflicts\n");
    wipe();
    make_tree();
    CHECK(fs::mkdirs("/dst/src/sub"));
    put("/dst/src/a.txt", "old");
    put("/dst/src/keep.txt", "mine");

    // Skip (no hook): nothing changes.
    fs::OpResult r = fs::copy({"/src"}, "/dst", fs::OpHooks());
    CHECK(r.skipped == 1 && get("/dst/src/a.txt") == "old");

    // Merge folders, replace files.
    std::vector<std::string> asked;
    fs::OpHooks h;
    h.conflict = [&](const std::string& t, bool sd, bool dd) {
        asked.push_back(t);
        if (sd && dd) return fs::Conflict::Merge;
        return fs::Conflict::Replace;
    };
    r = fs::copy({"/src"}, "/dst", h);
    CHECK(r.ok());
    CHECK(get("/dst/src/a.txt") == "alpha");
    CHECK(get("/dst/src/keep.txt") == "mine");            // merge keeps extra files
    CHECK(get("/dst/src/sub/b.bin") == get("/src/sub/b.bin"));
    CHECK(asked.size() >= 3);                             // /dst/src, a.txt, sub

    // Keep both.
    h.conflict = [](const std::string&, bool, bool) { return fs::Conflict::KeepBoth; };
    r = fs::copy({"/src/a.txt"}, "/dst/src", h);
    CHECK(r.ok() && get("/dst/src/a (2).txt") == "alpha");

    // Cancel stops everything.
    h.conflict = [](const std::string&, bool, bool) { return fs::Conflict::Cancel; };
    r = fs::copy({"/src/a.txt", "/src/sub"}, "/dst/src", h);
    CHECK(r.cancelled && r.done == 0);

    // Replacing a folder with a file of the same name.
    CHECK(fs::mkdirs("/x/thing/inner"));
    put("/thing", "file");
    h.conflict = [](const std::string&, bool sd, bool dd) { return (!sd && dd) ? fs::Conflict::Replace : fs::Conflict::Skip; };
    r = fs::copy({"/thing"}, "/x", h);
    CHECK(r.ok() && get("/x/thing") == "file");

    // Replacing the folder that contains the source is refused.
    CHECK(fs::mkdirs("/p/p"));
    put("/p/p/f", "1");
    h.conflict = [](const std::string&, bool, bool) { return fs::Conflict::Replace; };
    r = fs::move({"/p/p"}, "/", h);
    CHECK(r.failed == 1 && get("/p/p/f") == "1");
}

static void test_move() {
    fprintf(stderr, "[fs_ops] move\n");
    wipe();
    make_tree();
    CHECK(fs::mkdir("/dst"));
    fs::OpResult r = fs::move({"/src/a.txt", "/src/sub"}, "/dst", fs::OpHooks());
    CHECK(r.ok() && r.done == 2);
    CHECK(!fs::exists("/src/a.txt") && !fs::exists("/src/sub"));
    CHECK(get("/dst/a.txt") == "alpha" && fs::is_dir("/dst/sub/empty"));

    // Moving into the folder it is already in does nothing.
    r = fs::move({"/dst/a.txt"}, "/dst", fs::OpHooks());
    CHECK(r.skipped == 1 && get("/dst/a.txt") == "alpha");

    // Move with merge: source folder disappears once emptied.
    CHECK(fs::mkdirs("/m/data"));
    put("/m/data/one", "1");
    CHECK(fs::mkdirs("/dst/data"));
    put("/dst/data/two", "2");
    fs::OpHooks h;
    h.conflict = [](const std::string&, bool sd, bool dd) { return sd && dd ? fs::Conflict::Merge : fs::Conflict::Skip; };
    r = fs::move({"/m/data"}, "/dst", h);
    CHECK(r.ok() && get("/dst/data/one") == "1" && get("/dst/data/two") == "2" && !fs::exists("/m/data"));

    // Move into itself is refused.
    r = fs::move({"/dst"}, "/dst/sub", fs::OpHooks());
    CHECK(r.failed == 1 && fs::is_dir("/dst/sub"));
}

static void test_remove_and_cancel() {
    fprintf(stderr, "[fs_ops] remove + cancel\n");
    wipe();
    make_tree();
    fs::OpResult r = fs::remove_all({"/"}, fs::OpHooks());
    CHECK(r.failed == 1 && fs::exists("/src"));

    // Cancel during a copy: the partial file is removed.
    CHECK(fs::mkdir("/dst"));
    fs::OpHooks h;
    h.buffer_bytes = 1024;
    h.progress_ms = 0;
    uint64_t seen = 0;
    h.progress = [&](uint64_t done, uint64_t, const std::string&) {
        seen = done;
        return done < 50 * 1024;   // stop half way through the 100 KB file
    };
    r = fs::copy({"/src/sub/b.bin"}, "/dst", h);
    CHECK(r.cancelled && seen >= 50 * 1024 && !fs::exists("/dst/b.bin"));

    r = fs::remove_all({"/src", "/dst"}, fs::OpHooks());
    CHECK(r.ok() && r.done == 2 && !fs::exists("/src") && !fs::exists("/dst"));
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "fs_ops_sd";
    fs::set_root(root);
    CHECK(fs::mount());
    test_names();
    test_scan_copy();
    test_conflicts();
    test_move();
    test_remove_and_cancel();
    wipe();
    fprintf(stderr, "[fs_ops] %d checks passed\n", g_checks);
    return 0;
}
