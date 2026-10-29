// Host test for pd_git: clones a real repository from the network into a
// scratch "card" folder. The calling script then checks the result with
// the real git (fsck, status, log).
//   git_clone_test <card-root> <url> [depth] [branch]
#include <cstdio>
#include <cstdlib>
#include <string>

#include "pd/fs.h"
#include "pd/git.h"
#include "pd/net.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: git_clone_test <card-root> <url> [depth] [branch]\n");
        return 2;
    }
    pd::fs::set_root(argv[1]);
    if (!pd::fs::mount()) return 2;
    // The host's simulated radio: join its fake network (real sockets underneath).
    std::string err;
    if (!pd::net::start(&err) || !pd::net::connect("HomeNet", "secret", 1000, nullptr, &err)) {
        fprintf(stderr, "net: %s\n", err.c_str());
        return 2;
    }
    pd::git::CloneOptions o;
    o.url = argv[2];
    o.parent_dir = "/";
    if (argc > 3) o.depth = atoi(argv[3]);
    if (argc > 4) o.branch = argv[4];
    // Small limits so the file-backed paths (large objects, cache misses) run too.
    if (getenv("PD_GIT_SMALL")) pd::git::limits() = {200000, 4096, 16384};
    pd::git::CloneHooks h;
    int last = -1;
    h.progress = [&](pd::git::Phase p, uint64_t d, uint64_t t, const std::string&) {
        if (int(p) != last) fprintf(stderr, "[git] %s\n", pd::git::phase_name(p));
        last = int(p);
        (void)d;
        (void)t;
        return true;
    };
    const pd::git::CloneResult r = pd::git::clone(o, h);
    if (!r.ok) {
        fprintf(stderr, "[git] FAILED: %s\n", r.error.c_str());
        return 1;
    }
    fprintf(stderr, "[git] ok: %s  branch=%s commit=%s files=%u skipped=%u objects=%u pack=%llu bytes\n", r.dir.c_str(),
            r.branch.c_str(), r.commit.c_str(), r.files, r.skipped, r.objects, (unsigned long long)r.pack_bytes);
    printf("%s\n", r.dir.c_str());
    return 0;
}
