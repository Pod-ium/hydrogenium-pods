// Scan (HCI inquiry) state shared by the pd_bt backends (bt_cyw43.cpp,
// bt_sim.cpp); host-tested in tests/bt_scan_test.cpp.
//
// The CYW43 refuses a second HCI Inquiry while one is running (Command
// Disallowed, 0x0C) and Inquiry Cancel is unreliable there, so a backend
// never sends a second Inquiry: request() while a search is active just
// extends it and returns false instead of letting the caller fail.
#pragma once
#include <cstdint>
#include <string>

namespace pd::bt::detail {

struct ScanState {
    bool active = false;     // an inquiry is running (sent, not yet complete)
    uint32_t deadline = 0;   // stop waiting for it at this time (ms since boot)
    std::string error;       // why the last search stopped early ("" = it did not)

    static bool reached(uint32_t now, uint32_t t) { return int32_t(now - t) >= 0; }

    // A new search is requested at `now`. Returns true if the backend must
    // send a new inquiry to the radio; false when one is already running and
    // only the deadline was extended.
    bool request(uint32_t now, uint32_t timeout_ms) {
        error.clear();
        if (active) {
            deadline = now + timeout_ms;
            return false;
        }
        active = true;
        deadline = now + timeout_ms;
        return true;
    }

    // The running inquiry ended normally (INQUIRY_COMPLETE).
    void complete() { active = false; }

    // The radio refused the inquiry (COMMAND_STATUS error): the search is
    // over and `why` says why.
    void refused(const std::string& why) {
        active = false;
        error = why;
    }

    // Ends the search now (host simulator only; the device backend leaves the
    // inquiry running on the radio and waits for it).
    void stop(uint32_t now) {
        if (active) deadline = now;
        active = false;
    }

    // Past the deadline the search is over; returns true if it just expired.
    bool expired(uint32_t now) {
        if (active && reached(now, deadline)) {
            active = false;
            return true;
        }
        return false;
    }
};

}  // namespace pd::bt::detail
