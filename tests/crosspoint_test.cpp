// Host test for the CrossPoint upload client, run against
// tests/mock_crosspoint.py (ctest: crosspoint).
#include <cassert>
#include <cstdio>
#include <string>

#include "pd/crosspoint.h"
#include "pd/fs.h"
using namespace pd;
namespace x3 = pd::crosspoint;

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8765/";
    const char* file = argc > 2 ? argv[2] : "rich.epub";
    x3::Target t;
    std::string err;
    assert(x3::parse_url("http://crosspoint.local/", &t, &err) && t.host == "crosspoint.local" && t.port == 80 && t.prefix.empty());
    assert(x3::parse_url("192.168.1.9:8080", &t, &err) && t.host == "192.168.1.9" && t.port == 8080);
    assert(x3::parse_url("http://x3/sub/", &t, &err) && t.prefix == "/sub");
    assert(!x3::parse_url("https://x3", &t, &err));
    assert(x3::url_encode("/My Books/è") == "/My%20Books/%C3%A8");

    assert(net::connect("HomeNet", "secret", 1000, nullptr, &err));
    assert(x3::parse_url(url, &t, &err));
    uint32_t ip = 0;
    assert(net::resolve(t.host, &ip, 1000, nullptr, &err));
    x3::Client c(t, ip);
    x3::Reply r;
    assert(c.status(&r, nullptr, &err) && r.status == 200);
    printf("status: %s\n", r.body.c_str());

    fs::File f;
    assert(f.open(file, fs::Mode::Read));
    const uint32_t size = f.size();
    x3::Source src = [&](uint32_t off, uint8_t* b, uint32_t n) -> int {
        if (!f.seek(off)) return -1;
        return f.read(b, n);
    };
    int calls = 0;
    auto prog = [&](uint32_t, uint32_t) { ++calls; return true; };
    const std::string name = "Il mio racconto è.epub";
    c.remove("/Books/" + name, &r, nullptr, &err);  // leftovers from a previous run
    assert(c.upload("/Books", name, size, src, &r, prog, &err));
    printf("upload: %d %s\n", r.status, r.body.c_str());
    assert(r.status == 200);
    assert(c.upload("/Books", name, size, src, &r, prog, &err));
    printf("again: %d %s\n", r.status, r.body.c_str());
    assert(x3::is_already_exists(r));
    assert(c.remove("/Books/" + name, &r, nullptr, &err) && r.status == 200);
    assert(c.upload("/Books", name, size, src, &r, prog, &err) && r.status == 200);
    printf("x3 client tests passed (%d progress calls)\n", calls);
    return 0;
}
