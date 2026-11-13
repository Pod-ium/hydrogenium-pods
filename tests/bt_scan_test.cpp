// Host test for the pd_bt scan state machine (lib/services/bt/src/bt_scan.h).
// Asking for a search while one is running must extend it, not fail: the
// CYW43 refuses a second HCI Inquiry with Command Disallowed (0x0C), which
// pd::bt::scan_start() reported as "search could not start (0x0C)" — the
// "Search failed" seen when re-scanning right after a cancelled search.
#include <cstdio>
#include <cstdlib>

#include "../lib/services/bt/src/bt_scan.h"

using pd::bt::detail::ScanState;

static int g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

int main() {
    ScanState s;
    CHECK(!s.active);
    CHECK(s.error.empty());

    // First request: a new inquiry must be sent to the radio.
    CHECK(s.request(1000, 15000));
    CHECK(s.active);
    CHECK(s.deadline == 16000);
    CHECK(s.error.empty());

    // Second request while the first runs: no new inquiry, deadline
    // extended, no error.
    CHECK(!s.request(3000, 15000));
    CHECK(s.active);
    CHECK(s.deadline == 18000);
    CHECK(s.error.empty());

    // Still running before the deadline, over at the deadline.
    CHECK(!s.expired(17999));
    CHECK(s.active);
    CHECK(s.expired(18000));
    CHECK(!s.active);

    // A refused inquiry ends the search with an error...
    CHECK(s.request(20000, 15000));
    s.refused("the radio refused to search (status 0x0C)");
    CHECK(!s.active);
    CHECK(s.error == "the radio refused to search (status 0x0C)");
    // ...and a new search can start afterwards, with the error cleared.
    CHECK(s.request(40000, 15000));
    CHECK(s.active);
    CHECK(s.error.empty());

    // stop() ends the search immediately (the host simulator cancels; the
    // device backend leaves the inquiry running and waits for it).
    s.stop(41000);
    CHECK(!s.active);
    CHECK(ScanState::reached(41000, s.deadline));

    // complete() ends a running search without an error.
    CHECK(s.request(50000, 15000));
    s.complete();
    CHECK(!s.active);
    CHECK(s.error.empty());

    // Wrap-around safety of reached().
    CHECK(ScanState::reached(0xFFFFFFF0u, 0xFFFFFF00u));
    CHECK(ScanState::reached(0x00000010u, 0xFFFFFF00u));
    CHECK(!ScanState::reached(0xFFFFFFF0u, 0x00000010u));

    fprintf(stderr, "[bt_scan] %d checks passed\n", g_checks);
    return 0;
}
