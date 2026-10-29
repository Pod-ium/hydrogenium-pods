#include <cassert>
#include <cstdio>
#include <string>
#include "pd/fs.h"
#include "pd/text/text_buffer.h"
namespace pd::fs { extern int g_fail_read_opens; }
using namespace pd;
using pd::text::TextBuffer;
static std::string slurp(const char* p){ FILE* f=fopen(p,"rb"); std::string s; int c; while((c=fgetc(f))!=EOF) s+=char(c); fclose(f); return s; }
int main(){
  (void)!system("rm -rf /tmp/rt && mkdir -p /tmp/rt/w");
  TextBuffer tb; assert(tb.init("/tmp/rt/w"));
  tb.insert(0,"hello world",11);
  fs::g_fail_read_opens = 2;   // transient: retries must cover it
  assert(tb.saveTo("/tmp/rt/a.txt", nullptr));
  assert(!tb.reopenFailed() && tb.size()==11 && tb.path()=="/tmp/rt/a.txt");
  tb.insert(5,",",1);
  fs::g_fail_read_opens = 100; // persistent failure
  bool ok = tb.saveTo("/tmp/rt/a.txt", nullptr);
  fs::g_fail_read_opens = 0;
  assert(!ok && tb.reopenFailed());
  assert(tb.size()==0 && tb.path().empty() && !tb.modified());
  assert(slurp("/tmp/rt/a.txt")=="hello, world");
  printf("reopen test: %s\n", tb.lastError().c_str());
  assert(tb.load("/tmp/rt/a.txt") && tb.size()==12);
  printf("REOPEN TESTS PASSED\n");
}
