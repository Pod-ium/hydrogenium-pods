// Host test for the ePub exporter: converts sample documents into .epub files
// (checked afterwards with epubcheck and CrossPoint-like parsing by run_epub.sh).
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>

#include "pd/epub.h"
using namespace pd;

static std::string readFile(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: epubtest <input> <output.epub> [title]\n");
        return 2;
    }
    const std::string doc = readFile(argv[1]);
    epub::Reader rd = [&](uint32_t pos, uint8_t* buf, uint32_t n) -> uint32_t {
        if (pos >= doc.size()) return 0;
        n = std::min<uint32_t>(n, uint32_t(doc.size() - pos));
        memcpy(buf, doc.data() + pos, n);
        return n;
    };
    epub::Options o;
    o.kind = epub::kindForPath(argv[1]);
    o.title = argc > 3 ? argv[3] : epub::guessTitle(rd, uint32_t(doc.size()), o.kind, "Untitled");
    o.author = "Test Author";
    o.language = "it";
    std::string err;
    int calls = 0;
    const bool ok = epub::write(rd, uint32_t(doc.size()), o, argv[2], &err,
                                [&](uint32_t, uint32_t) { ++calls; return true; });
    printf("%s: %s (title \"%s\", %d progress calls)\n", argv[2], ok ? "ok" : err.c_str(),
           o.title.c_str(), calls);
    return ok ? 0 : 1;
}
