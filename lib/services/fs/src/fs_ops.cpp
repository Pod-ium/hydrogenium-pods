// pd::fs tree operations (copy / move / delete), on top of the pd::fs API.
#include "pd/fs_ops.h"

#include <cstdlib>
#include <cstring>

#include "pd/path.h"
#include "pd/sys.h"

namespace pd::fs {

namespace {

constexpr uint64_t kEntryCost = 512;          // progress units per file or folder
constexpr size_t kMaxListed = 20000;          // entries read from one folder

char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

bool equal_nocase(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}

std::string strip_slash(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s.empty() ? "/" : s;
}

struct Op {
    const OpHooks& hooks;
    OpResult r;
    uint64_t done = 0, total = 0;
    uint32_t last_report = 0;
    std::vector<uint8_t> buf;

    explicit Op(const OpHooks& h) : hooks(h) {}

    bool stopped() const { return r.cancelled; }

    // Adds work and reports it (throttled). Sets cancelled if the hook says so.
    void tick(uint64_t units, const std::string& current, bool force = false) {
        done += units;
        if (done > total) total = done;
        if (!hooks.progress || r.cancelled) return;
        const uint32_t now = sys::now_ms();
        if (!force && now - last_report < hooks.progress_ms) return;
        last_report = now;
        if (!hooks.progress(done, total, current)) r.cancelled = true;
    }

    void fail(const std::string& p, const char* why = nullptr) {
        ++r.failed;
        if (r.error.empty()) r.error = path::base_name(p) + ": " + (why ? why : last_error_text());
    }

    uint64_t cost_of(const std::string& p) {
        TreeStats st;
        scan(p, st);
        return st.bytes + (uint64_t(st.files) + st.dirs) * kEntryCost;
    }
};

bool list_all(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    return list_dir(dir.c_str(), out, kMaxListed, true);
}

// Removes one file or empty folder, clearing read-only first if needed.
bool remove_one(const std::string& p) {
    if (remove(p.c_str())) return true;
    return make_writable(p.c_str()) && remove(p.c_str());
}

// Deletes p (file or folder tree). Counts entries in op.r.done only when
// count is set (top-level delete); a Replace during copy does not count.
bool remove_item(Op& op, const std::string& p, bool count) {
    Info in;
    if (!stat(p.c_str(), &in)) {
        op.fail(p);
        return false;
    }
    if (in.is_dir) {
        std::vector<DirEntry> kids;
        if (!list_all(p, kids)) {
            op.fail(p);
            return false;
        }
        bool all = true;
        for (const DirEntry& e : kids) {
            if (op.stopped()) return false;
            if (!remove_item(op, path::join(p, e.name), false)) all = false;
        }
        if (!all) return false;
    }
    if (!remove_one(p)) {
        op.fail(p);
        return false;
    }
    if (count) ++op.r.done;
    op.tick(kEntryCost, p);
    return true;
}

bool copy_file(Op& op, const std::string& from, const std::string& to) {
    File in, out;
    if (!in.open(from, Mode::Read)) {
        op.fail(from);
        return false;
    }
    if (!out.open(to, Mode::Write)) {
        op.fail(to);
        return false;
    }
    const uint32_t chunk = uint32_t(op.buf.size());
    bool ok = true;
    for (;;) {
        const int n = in.read(op.buf.data(), chunk);
        if (n < 0) {
            op.fail(from);
            ok = false;
            break;
        }
        if (n == 0) break;
        const int w = out.write(op.buf.data(), uint32_t(n));
        if (w != n) {
            op.fail(to, w < 0 ? nullptr : "disk full");
            ok = false;
            break;
        }
        op.tick(uint64_t(n), from);
        if (op.stopped()) {
            ok = false;
            break;
        }
        if (uint32_t(n) < chunk) break;
    }
    in.close();
    if (ok && !out.sync()) {
        op.fail(to);
        ok = false;
    }
    out.close();
    if (!ok) {
        remove(to.c_str());
        return false;
    }
    copy_metadata(from.c_str(), to.c_str());   // best effort
    op.tick(kEntryCost, from);
    return true;
}

// Copies or moves `src` into folder `dst_dir`. known_cost: the progress units
// of src when already known (top level), 0 = work it out if needed.
void transfer(Op& op, const std::string& src, const std::string& dst_dir, bool move, uint64_t known_cost) {
    if (op.stopped()) return;
    auto cost = [&]() { return known_cost ? known_cost : op.cost_of(src); };

    Info si;
    if (!stat(src.c_str(), &si)) {
        op.fail(src);
        return;
    }
    const std::string name = path::base_name(src);
    std::string target = path::join(dst_dir, name);

    if (si.is_dir && same_or_inside(dst_dir, src)) {
        op.fail(src, "cannot go inside itself");
        op.tick(cost(), src);
        return;
    }
    if (same_path(src, target)) {
        if (move) {   // already there
            ++op.r.skipped;
            op.tick(cost(), src);
            return;
        }
        target = path::join(dst_dir, unique_name(dst_dir, name, si.is_dir));
    }

    Info ti;
    if (stat(target.c_str(), &ti)) {
        Conflict c = op.hooks.conflict ? op.hooks.conflict(target, si.is_dir, ti.is_dir) : Conflict::Skip;
        if (c == Conflict::Merge && !(si.is_dir && ti.is_dir)) c = Conflict::Skip;
        switch (c) {
            case Conflict::Cancel:
                op.r.cancelled = true;
                return;
            case Conflict::Skip:
                ++op.r.skipped;
                op.tick(cost(), src, true);
                return;
            case Conflict::KeepBoth:
                target = path::join(dst_dir, unique_name(dst_dir, name, si.is_dir));
                break;
            case Conflict::Replace:
                if (same_or_inside(src, target)) {   // e.g. /a/a pasted into /
                    op.fail(src, "would delete the source");
                    op.tick(cost(), src);
                    return;
                }
                if (!remove_item(op, target, false)) {
                    op.tick(cost(), src);
                    return;
                }
                break;
            case Conflict::Merge: {
                std::vector<DirEntry> kids;
                if (!list_all(src, kids)) {
                    op.fail(src);
                    return;
                }
                for (const DirEntry& e : kids) {
                    if (op.stopped()) return;
                    transfer(op, path::join(src, e.name), target, move, 0);
                }
                if (move) {
                    std::vector<DirEntry> left;
                    if (list_all(src, left) && left.empty()) remove_one(src);
                }
                ++op.r.done;
                op.tick(kEntryCost, src, true);
                return;
            }
        }
    }

    if (move && rename(src.c_str(), target.c_str())) {
        ++op.r.done;
        op.tick(cost(), src);
        return;
    }

    if (si.is_dir) {
        if (!mkdir(target.c_str())) {
            op.fail(target);
            op.tick(cost(), src);
            return;
        }
        op.tick(kEntryCost, src);
        std::vector<DirEntry> kids;
        if (!list_all(src, kids)) {
            op.fail(src);
            return;
        }
        const uint32_t failed_before = op.r.failed;
        for (const DirEntry& e : kids) {
            if (op.stopped()) return;
            transfer(op, path::join(src, e.name), target, move, 0);
        }
        if (op.stopped()) return;
        copy_metadata(src.c_str(), target.c_str());
        if (move) {
            std::vector<DirEntry> left;
            if (op.r.failed == failed_before && list_all(src, left) && left.empty() && !remove_one(src))
                op.fail(src);
        }
        ++op.r.done;
        return;
    }

    if (!copy_file(op, src, target)) return;
    if (move && !remove_one(src)) {
        op.fail(src);
        return;
    }
    ++op.r.done;
}

OpResult run_transfer(const std::vector<std::string>& sources, const std::string& dst_dir, bool move,
                      const OpHooks& hooks) {
    Op op(hooks);
    op.buf.resize(hooks.buffer_bytes ? hooks.buffer_bytes : 4096);
    const std::string dst = strip_slash(dst_dir);
    if (!is_dir(dst.c_str())) {
        op.fail(dst, "destination folder not found");
        return op.r;
    }
    std::vector<uint64_t> costs;
    for (const std::string& s : sources) {
        const uint64_t c = op.cost_of(strip_slash(s));
        costs.push_back(c ? c : kEntryCost);
        op.total += costs.back();
    }
    op.tick(0, sources.empty() ? std::string() : sources[0], true);
    for (size_t i = 0; i < sources.size() && !op.stopped(); ++i) transfer(op, strip_slash(sources[i]), dst, move, costs[i]);
    if (!op.stopped()) op.tick(0, "", true);
    return op.r;
}

}  // namespace

// ---------------------------------------------------------------- names
bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 255 || n == "." || n == "..") return false;
    for (unsigned char c : n) {
        if (c < 0x20 || strchr("\"*/:<>?\\|", c)) return false;
    }
    return n.back() != '.' && n.back() != ' ';
}

bool same_path(const std::string& a, const std::string& b) {
    const std::string x = strip_slash(a), y = strip_slash(b);
    return x.size() == y.size() && equal_nocase(x.data(), y.data(), x.size());
}

bool same_or_inside(const std::string& p, const std::string& d) {
    const std::string x = strip_slash(p), y = strip_slash(d);
    if (y == "/") return true;
    if (x.size() < y.size() || !equal_nocase(x.data(), y.data(), y.size())) return false;
    return x.size() == y.size() || x[y.size()] == '/';
}

std::string unique_name(const std::string& dir, const std::string& name, bool is_dir) {
    if (!exists(path::join(dir, name).c_str())) return name;
    std::string stem = name, ext;
    if (!is_dir) {
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
    }
    // "report (2)" -> continue from 3 instead of making "report (2) (2)"
    int start = 2;
    const size_t open = stem.rfind(" (");
    if (open != std::string::npos && stem.back() == ')') {
        const std::string num = stem.substr(open + 2, stem.size() - open - 3);
        if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos && num.size() < 6) {
            start = atoi(num.c_str()) + 1;
            stem = stem.substr(0, open);
        }
    }
    for (int i = start; i < 100000; ++i) {
        const std::string cand = stem + " (" + std::to_string(i) + ")" + ext;
        if (!exists(path::join(dir, cand).c_str())) return cand;
    }
    return name;
}

// ---------------------------------------------------------------- sizes
bool scan(const std::string& p, TreeStats& out, const std::function<bool()>& keep_going) {
    Info in;
    if (!stat(p.c_str(), &in)) return false;
    if (!in.is_dir) {
        ++out.files;
        out.bytes += in.size;
        return true;
    }
    ++out.dirs;
    std::vector<DirEntry> kids;
    if (!list_all(p, kids)) return false;
    for (const DirEntry& e : kids) {
        if (keep_going && !keep_going()) return false;
        if (e.is_dir) {
            if (!scan(path::join(p, e.name), out, keep_going)) return false;
        } else {
            ++out.files;
            out.bytes += e.size;
        }
    }
    return true;
}

// ---------------------------------------------------------------- operations
OpResult copy(const std::vector<std::string>& sources, const std::string& dst_dir, const OpHooks& hooks) {
    return run_transfer(sources, dst_dir, false, hooks);
}

OpResult move(const std::vector<std::string>& sources, const std::string& dst_dir, const OpHooks& hooks) {
    return run_transfer(sources, dst_dir, true, hooks);
}

OpResult remove_all(const std::vector<std::string>& paths, const OpHooks& hooks) {
    Op op(hooks);
    for (const std::string& p : paths) {
        TreeStats st;
        scan(strip_slash(p), st);
        op.total += (uint64_t(st.files) + st.dirs) * kEntryCost;
    }
    op.tick(0, paths.empty() ? std::string() : paths[0], true);
    for (const std::string& raw : paths) {
        if (op.stopped()) break;
        const std::string p = strip_slash(raw);
        if (p == "/") {
            op.fail(p, "cannot delete the root folder");
            continue;
        }
        remove_item(op, p, true);
    }
    if (!op.stopped()) op.tick(0, "", true);
    return op.r;
}

}  // namespace pd::fs
