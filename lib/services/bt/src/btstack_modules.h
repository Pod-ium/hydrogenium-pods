// pd_bt internal: BTstack module set-up and tear-down shared by the device
// glue and the host lifecycle test.
#pragma once

namespace pd::bt::detail {
// Resets the static lists of every BTstack module pd_bt uses (L2CAP, SDP,
// RFCOMM, GOEP). Must run after hci_close() and before btstack_memory_deinit():
// that frees the objects those lists point to, and the modules' init
// functions do not clear them, so the next start would walk freed memory.
void btstack_modules_deinit();
// The SDP record of our Object Push service (what a phone reads to find us).
// Returns its length; buf must hold at least 160 bytes.
unsigned build_opp_sdp_record(unsigned char* buf, unsigned buf_len, unsigned long handle, unsigned char rfcomm_channel);
}  // namespace pd::bt::detail
