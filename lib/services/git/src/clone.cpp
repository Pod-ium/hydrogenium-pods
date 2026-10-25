// pd_git: clone over smart HTTP (protocol v0, shallow by default).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pack.h"
#include "pd/fs_ops.h"
#include "pd/git.h"
#include "pd/path.h"
#include "pd/tls.h"

namespace pd::git {

using detail::Content;
using detail::Pack;
using detail::Sha;
using detail::Sha1;

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Connecting: return "Connecting";
        case Phase::Receiving: return "Receiving";
        case Phase::Indexing: return "Indexing";
        case Phase::Resolving: return "Resolving deltas";
        case Phase::CheckingOut: return "Checking out";
    }
    return "";
}

std::string normalize_url(const std::string& in) {
    std::string u = in;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    while (!u.empty() && u[0] == ' ') u.erase(0, 1);
    if (u.empty()) return "";
    if (u.compare(0, 8, "https://") != 0 && u.compare(0, 7, "http://") != 0) {
        const size_t slash = u.find('/');
        if (slash == std::string::npos) return "";
        const std::string first = u.substr(0, slash);
        if (first.find('.') != std::string::npos) u = "https://" + u;   // host/owner/repo
        else u = "https://github.com/" + u;                             // owner/repo
    }
    net::Url p;
    if (!net::parse_url(u, p) || p.path.size() < 2) return "";
    if (u.size() < 4 || u.compare(u.size() - 4, 4, ".git") != 0) u += ".git";
    return u;
}

std::string repo_name(const std::string& url) {
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() > 4 && u.compare(u.size() - 4, 4, ".git") == 0) u.resize(u.size() - 4);
    const size_t s = u.find_last_of("/:");
    return s == std::string::npos ? u : u.substr(s + 1);
}

namespace {

const char* const kUserAgent = "git/2.45.2 (picopods)";

// Reads pkt-lines from an HTTP body.
class PktReader {
public:
    explicit PktReader(net::HttpClient& h) : h_(h) {}
    // 1 = data (in `out`), 0 = flush-pkt, -1 = error / end
    int next(std::string& out) {
        char len[4];
        if (!exact(len, 4)) return -1;
        int n = 0;
        for (char c : len) {
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return -1;
            n = n * 16 + v;
        }
        if (n == 0) return 0;
        if (n == 1 || n == 2) return 0;   // delim / response-end (v2): treat like flush
        if (n < 4) return -1;
        out.resize(size_t(n - 4));
        if (n > 4 && !exact(&out[0], size_t(n - 4))) return -1;
        return 1;
    }

private:
    bool exact(char* d, size_t n) {
        while (n) {
            const int r = h_.read(d, uint32_t(n));
            if (r <= 0) return false;
            d += r;
            n -= size_t(r);
        }
        return true;
    }
    net::HttpClient& h_;
};

std::string pkt(const std::string& s) {
    char b[8];
    snprintf(b, sizeof b, "%04x", unsigned(s.size() + 4));
    return b + s;
}

bool fat_ok(const std::string& name) { return fs::valid_name(name); }

struct Ctx {
    const CloneOptions& opt;
    const CloneHooks& hooks;
    CloneResult& r;
    std::string url, dir, git, tmp;
    bool cancelled = false;

    bool report(Phase p, uint64_t done, uint64_t total, const std::string& detail = std::string()) {
        if (hooks.progress && !hooks.progress(p, done, total, detail)) cancelled = true;
        return !cancelled;
    }
    net::KeepGoing keep_going() {
        return [this]() {
            if (hooks.progress && !hooks.progress(Phase::Connecting, 0, 0, "")) cancelled = true;
            return !cancelled;
        };
    }
};

bool write_text(const std::string& path, const std::string& text) { return fs::write_file(path.c_str(), text, false); }

// ---------------------------------------------------------------- refs
struct Advert {
    std::map<std::string, Sha> refs;
    std::string caps;
    std::string head_target;   // from symref=HEAD:refs/heads/x
};

bool discover(Ctx& c, Advert& ad) {
    net::HttpClient http;
    net::HttpRequest rq;
    rq.url = c.url + "/info/refs?service=git-upload-pack";
    rq.headers = {{"User-Agent", kUserAgent}, {"Accept", "*/*"}, {"Pragma", "no-cache"}};
    std::string err;
    c.report(Phase::Connecting, 0, 0, "Connecting to the server");
    if (!http.open(rq, c.keep_going(), &err)) {
        c.r.error = c.cancelled ? "cancelled" : err;
        return false;
    }
    if (http.status() == 401 || http.status() == 403 || http.status() == 404) {
        c.r.error = "repository not found, or private (sign-in is not supported yet)";
        return false;
    }
    if (http.status() != 200) {
        c.r.error = "server answered HTTP " + std::to_string(http.status());
        return false;
    }
    if (http.header("Content-Type").find("git-upload-pack-advertisement") == std::string::npos) {
        c.r.error = "not a Git server (no smart HTTP support)";
        return false;
    }
    PktReader pr(http);
    std::string line;
    bool first = true;
    int r;
    while ((r = pr.next(line)) >= 0) {
        if (r == 0) {
            if (!ad.refs.empty() || !first) {
                if (!ad.refs.empty()) break;
            }
            continue;
        }
        if (line.compare(0, 1, "#") == 0) continue;   // "# service=git-upload-pack"
        if (!line.empty() && line.back() == '\n') line.pop_back();
        const size_t nul = line.find('\0');
        if (nul != std::string::npos) {
            ad.caps = line.substr(nul + 1);
            line.resize(nul);
        }
        first = false;
        if (line.size() < 42) continue;
        Sha id;
        if (!id.from_hex(line.c_str())) continue;
        ad.refs[line.substr(41)] = id;
    }
    const size_t sym = ad.caps.find("symref=HEAD:");
    if (sym != std::string::npos) {
        const size_t end = ad.caps.find(' ', sym);
        ad.head_target = ad.caps.substr(sym + 12, end == std::string::npos ? std::string::npos : end - sym - 12);
    }
    if (ad.refs.empty()) {
        c.r.error = "the repository is empty";
        return false;
    }
    return true;
}

bool has_cap(const std::string& caps, const char* cap) {
    const size_t n = strlen(cap);
    size_t p = 0;
    while ((p = caps.find(cap, p)) != std::string::npos) {
        const bool start = p == 0 || caps[p - 1] == ' ';
        const bool end = p + n == caps.size() || caps[p + n] == ' ' || caps[p + n] == '=';
        if (start && end) return true;
        p += n;
    }
    return false;
}

// ---------------------------------------------------------------- fetch
bool fetch_pack(Ctx& c, const Sha& want, const std::string& caps, const std::string& pack_path, Sha& pack_sha,
                std::vector<Sha>& shallow) {
    const bool sb64 = has_cap(caps, "side-band-64k");
    const bool sb = sb64 || has_cap(caps, "side-band");
    std::string ask = "want " + want.hex();
    if (sb64) ask += " side-band-64k";
    else if (sb) ask += " side-band";
    if (has_cap(caps, "ofs-delta")) ask += " ofs-delta";
    if (c.opt.depth > 0 && has_cap(caps, "shallow")) ask += " shallow";
    if (has_cap(caps, "no-progress")) ask += " no-progress";
    ask += " agent=picopods/1.0\n";
    std::string body = pkt(ask);
    if (c.opt.depth > 0) body += pkt("deepen " + std::to_string(c.opt.depth) + "\n");
    body += "0000";
    body += pkt("done\n");

    net::HttpClient http;
    net::HttpRequest rq;
    rq.method = "POST";
    rq.url = c.url + "/git-upload-pack";
    rq.headers = {{"User-Agent", kUserAgent},
                  {"Content-Type", "application/x-git-upload-pack-request"},
                  {"Accept", "application/x-git-upload-pack-result"}};
    rq.body = body;
    std::string err;
    c.report(Phase::Connecting, 0, 0, "Asking for the files");
    if (!http.open(rq, c.keep_going(), &err)) {
        c.r.error = c.cancelled ? "cancelled" : err;
        return false;
    }
    if (http.status() != 200) {
        c.r.error = "server answered HTTP " + std::to_string(http.status()) + " to the fetch";
        return false;
    }

    fs::File out;
    if (!out.open(pack_path, fs::Mode::Write)) {
        c.r.error = "cannot create files on the card";
        return false;
    }
    // Pack checksum: SHA-1 of everything but the last 20 bytes, which hold it.
    Sha1 h;
    uint8_t tail[20];
    size_t tail_len = 0;
    uint64_t bytes = 0;
    auto take = [&](const uint8_t* d, size_t n) -> bool {
        if (out.write(d, uint32_t(n)) != int(n)) return false;
        bytes += n;
        // keep the last 20 bytes out of the hash until we know they are the trailer
        if (tail_len + n <= 20) {
            memcpy(tail + tail_len, d, n);
            tail_len += n;
            return true;
        }
        const size_t total = tail_len + n, hashable = total - 20;
        const size_t from_tail = std::min(hashable, tail_len);
        h.update(tail, from_tail);
        h.update(d, hashable - from_tail);
        uint8_t nt[20];
        const size_t keep_tail = tail_len - from_tail;
        memcpy(nt, tail + from_tail, keep_tail);
        memcpy(nt + keep_tail, d + (hashable - from_tail), 20 - keep_tail);
        memcpy(tail, nt, 20);
        tail_len = 20;
        return true;
    };

    PktReader pr(http);
    std::string line;
    bool in_pack = false;
    uint32_t next_report = 0;
    const int64_t total = http.content_length();
    for (;;) {
        if (!sb && in_pack) {   // no side-band: raw pack follows
            uint8_t b[2048];
            const int n = http.read(b, sizeof b);
            if (n < 0) {
                c.r.error = http.error();
                return false;
            }
            if (n == 0) break;
            if (!take(b, size_t(n))) {
                c.r.error = "write failed (card full?)";
                return false;
            }
            continue;
        }
        const int r = pr.next(line);
        if (r < 0) {
            if (in_pack) break;   // body ended
            c.r.error = http.error().empty() ? "the server's answer was cut short" : http.error();
            return false;
        }
        if (r == 0) {
            if (in_pack) break;
            continue;
        }
        if (!in_pack) {
            if (line.compare(0, 8, "shallow ") == 0 && line.size() >= 48) {
                Sha s;
                if (s.from_hex(line.c_str() + 8)) shallow.push_back(s);
                continue;
            }
            if (line.compare(0, 10, "unshallow ") == 0) continue;
            if (line.compare(0, 3, "NAK") == 0 || line.compare(0, 3, "ACK") == 0) {
                in_pack = true;
                continue;
            }
            if (line.compare(0, 3, "ERR") == 0) {
                c.r.error = "server: " + line.substr(4);
                return false;
            }
            continue;
        }
        const uint8_t band = uint8_t(line[0]);
        if (band == 1) {
            if (!take(reinterpret_cast<const uint8_t*>(line.data()) + 1, line.size() - 1)) {
                c.r.error = "write failed (card full?)";
                return false;
            }
        } else if (band == 3) {
            c.r.error = "server: " + line.substr(1);
            return false;
        }
        if (bytes >= next_report) {
            next_report = uint32_t(bytes) + 16384;
            if (!c.report(Phase::Receiving, bytes, total > 0 ? uint64_t(total) : 0)) {
                c.r.error = "cancelled";
                return false;
            }
        }
    }
    out.close();
    c.r.pack_bytes = bytes;
    if (bytes < 32 || tail_len != 20) {
        c.r.error = "no data received";
        return false;
    }
    pack_sha = h.finish();
    if (memcmp(pack_sha.b, tail, 20) != 0) {
        c.r.error = "download damaged (pack checksum mismatch)";
        return false;
    }
    c.report(Phase::Receiving, bytes, bytes);
    return true;
}

// ---------------------------------------------------------------- checkout
struct Checkout {
    Checkout(Ctx& ctx, Pack& p) : c(ctx), pack(p) {}
    Ctx& c;
    Pack& pack;
    fs::File index_tmp;   // index entries in order (Git's index order = tree walk order)
    uint32_t entries = 0;

    bool add_entry(const std::string& rel, const Sha& id, uint32_t mode, uint32_t size) {
        // ctime, mtime (sec, nsec) = 0; dev, ino, uid, gid = 0: Git re-reads
        // the file once and refreshes these.
        std::string e(40, '\0');
        auto be32 = [&](size_t at, uint32_t v) {
            e[at] = char(v >> 24);
            e[at + 1] = char(v >> 16);
            e[at + 2] = char(v >> 8);
            e[at + 3] = char(v);
        };
        be32(24, mode);
        be32(36, size);
        e.append(reinterpret_cast<const char*>(id.b), 20);
        const uint16_t flags = uint16_t(std::min<size_t>(rel.size(), 0xFFF));
        e += char(flags >> 8);
        e += char(flags & 0xFF);
        e += rel;
        const size_t pad = 8 - (e.size() % 8);   // 1..8 NULs
        e.append(pad, '\0');
        ++entries;
        return index_tmp.write(e.data(), uint32_t(e.size())) == int(e.size());
    }

    bool tree(const Sha& id, const std::string& dir, const std::string& rel) {
        uint32_t off;
        if (!pack.find(id, &off)) {
            c.r.error = "tree " + id.hex() + " missing from the pack";
            return false;
        }
        Content t;
        std::string err;
        if (!pack.load(off, t, "", &err) || t.type != detail::OBJ_TREE || t.in_file()) {
            c.r.error = err.empty() ? "bad tree object" : err;
            t.release();
            return false;
        }
        const std::vector<uint8_t> data = std::move(t.mem);
        size_t p = 0;
        while (p < data.size()) {
            const size_t sp = size_t(std::find(data.begin() + long(p), data.end(), ' ') - data.begin());
            const size_t nul = size_t(std::find(data.begin() + long(sp), data.end(), '\0') - data.begin());
            if (sp >= data.size() || nul >= data.size() || nul + 21 > data.size()) {
                c.r.error = "corrupt tree";
                return false;
            }
            const uint32_t mode = uint32_t(strtoul(std::string(data.begin() + long(p), data.begin() + long(sp)).c_str(), nullptr, 8));
            const std::string name(data.begin() + long(sp) + 1, data.begin() + long(nul));
            Sha e;
            memcpy(e.b, &data[nul + 1], 20);
            p = nul + 21;
            if (c.cancelled) return false;

            const std::string path = pd::path::join(dir, name);
            const std::string r = rel.empty() ? name : rel + "/" + name;
            if (!fat_ok(name) || name == ".git") {
                ++c.r.skipped;
                continue;
            }
            if (mode == 040000) {
                if (!fs::is_dir(path.c_str()) && !fs::mkdir(path.c_str())) {
                    c.r.error = "cannot create folder " + r;
                    return false;
                }
                if (!tree(e, path, r)) return false;
            } else if (mode == 0160000) {   // submodule: an empty folder, like git without --recursive
                fs::mkdir(path.c_str());
                ++c.r.skipped;
                if (!add_entry(r, e, mode, 0)) return false;
            } else {   // 100644, 100755, 120000 (symlink: a file holding the target)
                uint32_t boff;
                if (!pack.find(e, &boff)) {
                    c.r.error = "file " + r + " missing from the pack";
                    return false;
                }
                Content b;
                if (!pack.load(boff, b, path, &err)) {
                    c.r.error = r + ": " + err;
                    return false;
                }
                const uint32_t size = uint32_t(b.size);
                b.path.clear();   // the checked-out file stays
                ++c.r.files;
                if (!add_entry(r, e, mode == 0100755 ? 0100755 : mode == 0120000 ? 0120000 : 0100644, size))
                    return false;
                if (!c.report(Phase::CheckingOut, c.r.files, 0, r)) return false;
            }
        }
        return true;
    }
};

bool write_index(const std::string& tmp_entries, const std::string& index_path, uint32_t count) {
    fs::File in, out;
    if (!in.open(tmp_entries, fs::Mode::Read) || !out.open(index_path, fs::Mode::Write)) return false;
    Sha1 h;
    uint8_t head[12] = {'D', 'I', 'R', 'C', 0, 0, 0, 2,
                        uint8_t(count >> 24), uint8_t(count >> 16), uint8_t(count >> 8), uint8_t(count)};
    h.update(head, 12);
    bool ok = out.write(head, 12) == 12;
    std::vector<uint8_t> b(4096);
    int r;
    while (ok && (r = in.read(b.data(), 4096)) > 0) {
        h.update(b.data(), size_t(r));
        ok = out.write(b.data(), uint32_t(r)) == r;
    }
    const Sha s = h.finish();
    ok = ok && out.write(s.b, 20) == 20;
    return ok;
}

bool commit_tree(Pack& pack, const Sha& commit, Sha& tree, std::string& err) {
    uint32_t off;
    if (!pack.find(commit, &off)) {
        err = "the commit is missing from the pack";
        return false;
    }
    Content c;
    if (!pack.load(off, c, "", &err) || c.in_file()) return false;
    if (c.type == detail::OBJ_TAG) {   // annotated tag: follow "object"
        const std::string s(c.mem.begin(), c.mem.end());
        Sha target;
        if (s.compare(0, 7, "object ") != 0 || !target.from_hex(s.c_str() + 7)) {
            err = "bad tag object";
            return false;
        }
        return commit_tree(pack, target, tree, err);
    }
    const std::string s(c.mem.begin(), c.mem.end());
    if (c.type != detail::OBJ_COMMIT || s.compare(0, 5, "tree ") != 0 || s.size() < 45 || !tree.from_hex(s.c_str() + 5)) {
        err = "bad commit object";
        return false;
    }
    return true;
}

}  // namespace

CloneResult clone(const CloneOptions& opt, const CloneHooks& hooks) {
    CloneResult r;
    Ctx c{opt, hooks, r, "", "", "", ""};
    c.url = normalize_url(opt.url);
    if (c.url.empty()) {
        r.error = "not a repository URL";
        return r;
    }
    const std::string name = opt.dir_name.empty() ? repo_name(c.url) : opt.dir_name;
    if (!fs::valid_name(name)) {
        r.error = "cannot make a folder name from the URL";
        return r;
    }
    c.dir = pd::path::join(opt.parent_dir.empty() ? "/" : opt.parent_dir, name);
    r.dir = c.dir;
    if (fs::exists(c.dir.c_str())) {
        r.error = "\"" + name + "\" already exists here";
        return r;
    }

    Advert ad;
    if (!discover(c, ad)) {
        r.cancelled = c.cancelled;
        return r;
    }
    // Pick the branch.
    std::string ref;
    if (!opt.branch.empty()) {
        ref = "refs/heads/" + opt.branch;
        if (!ad.refs.count(ref)) {
            ref = "refs/tags/" + opt.branch;
            if (!ad.refs.count(ref)) {
                r.error = "no branch or tag named \"" + opt.branch + "\"";
                return r;
            }
        }
    } else if (!ad.head_target.empty() && ad.refs.count(ad.head_target)) {
        ref = ad.head_target;
    } else if (ad.refs.count("HEAD")) {
        for (const auto& kv : ad.refs)
            if (kv.first.compare(0, 11, "refs/heads/") == 0 && kv.second == ad.refs["HEAD"]) {
                ref = kv.first;
                break;
            }
        if (ref.empty()) ref = "HEAD";
    } else {
        ref = ad.refs.begin()->first;
    }
    const Sha want = ad.refs[ref];
    const bool is_branch = ref.compare(0, 11, "refs/heads/") == 0;
    r.branch = is_branch ? ref.substr(11) : ref.compare(0, 10, "refs/tags/") == 0 ? ref.substr(10) : "";

    // Folders.
    c.git = c.dir + "/.git";
    c.tmp = c.git + "/picopods-tmp";
    if (!fs::mkdirs((c.git + "/objects/pack").c_str()) || !fs::mkdirs((c.git + "/refs/heads").c_str()) ||
        !fs::mkdirs((c.git + "/refs/tags").c_str()) || !fs::mkdirs((c.git + "/refs/remotes/origin").c_str()) ||
        !fs::mkdirs(c.tmp.c_str()) || !fs::mkdirs((c.git + "/objects/info").c_str())) {
        r.error = "cannot create folders on the card";
        fs::remove_all({c.dir}, fs::OpHooks());
        return r;
    }
    auto fail = [&](const std::string& why) {
        if (r.error.empty()) r.error = why;
        r.cancelled = c.cancelled || r.error == "cancelled";
        fs::remove_all({c.dir}, fs::OpHooks());
        r.ok = false;
        return r;
    };

    const std::string tmp_pack = c.git + "/objects/pack/incoming.pack";
    Sha pack_sha;
    std::vector<Sha> shallow;
    if (!fetch_pack(c, want, ad.caps, tmp_pack, pack_sha, shallow)) return fail("download failed");

    Pack pack;
    std::string err;
    if (!pack.open(tmp_pack, c.tmp, &err)) return fail(err);
    r.objects = pack.count();
    if (!pack.index([&](uint32_t d, uint32_t t) { return c.report(Phase::Indexing, d, t); }, &err) ||
        !pack.resolve([&](uint32_t d, uint32_t t) { return c.report(Phase::Resolving, d, t); }, &err)) {
        pack.close();
        return fail(err);
    }
    const std::string base = c.git + "/objects/pack/pack-" + pack_sha.hex();
    if (!pack.write_idx(base + ".idx", pack_sha, &err)) {
        pack.close();
        return fail(err);
    }

    // Working tree.
    Sha tree;
    if (!commit_tree(pack, want, tree, err)) {
        pack.close();
        return fail(err);
    }
    Checkout co(c, pack);
    const std::string idx_tmp = c.tmp + "/index.entries";
    if (!co.index_tmp.open(idx_tmp, fs::Mode::Write)) {
        pack.close();
        return fail("cannot create files on the card");
    }
    c.report(Phase::CheckingOut, 0, 0, "");
    const bool co_ok = co.tree(tree, c.dir, "");
    co.index_tmp.close();
    pack.close();
    if (!co_ok) return fail(c.cancelled ? "cancelled" : "checkout failed");
    if (!fs::rename(tmp_pack.c_str(), (base + ".pack").c_str())) return fail("cannot rename the pack");

    // Git metadata.
    const std::string hex = want.hex();
    bool ok = write_index(idx_tmp, c.git + "/index", co.entries);
    if (is_branch) {
        ok = ok && write_text(c.git + "/HEAD", "ref: refs/heads/" + r.branch + "\n");
        ok = ok && write_text(c.git + "/refs/heads/" + r.branch, hex + "\n");
        ok = ok && write_text(c.git + "/refs/remotes/origin/" + r.branch, hex + "\n");
        ok = ok && write_text(c.git + "/refs/remotes/origin/HEAD", "ref: refs/remotes/origin/" + r.branch + "\n");
    } else {
        ok = ok && write_text(c.git + "/HEAD", hex + "\n");   // detached (tag)
        if (!r.branch.empty()) ok = ok && write_text(c.git + "/refs/tags/" + r.branch, hex + "\n");
    }
    std::string cfg =
        "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n\tlogallrefupdates = true\n"
        "\tsymlinks = false\n\tignorecase = true\n"
        "[remote \"origin\"]\n\turl = " + c.url + "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n";
    if (is_branch) cfg += "[branch \"" + r.branch + "\"]\n\tremote = origin\n\tmerge = refs/heads/" + r.branch + "\n";
    ok = ok && write_text(c.git + "/config", cfg);
    if (!shallow.empty()) {
        std::string s;
        for (const Sha& x : shallow) s += x.hex() + "\n";
        ok = ok && write_text(c.git + "/shallow", s);
    }
    ok = ok && write_text(c.git + "/description", "Cloned by PicoPods\n");
    fs::remove_all({c.tmp}, fs::OpHooks());
    if (!ok) return fail("cannot write the repository files (card full?)");
    r.commit = hex;
    r.ok = true;
    return r;
}

}  // namespace pd::git
