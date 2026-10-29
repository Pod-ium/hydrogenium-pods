// PicoPods - Bluetooth file transfer (OBEX Object Push), both directions.
//
// The standard "send file via Bluetooth": Android (Share -> Bluetooth),
// Ubuntu / GNOME (Settings -> Bluetooth -> Send Files; bluetooth-sendto),
// Windows. Not supported by iPhone / iPad.
//
//   pd::bt::start(&err);                 // radio on (shared with Wi-Fi)
//   pd::bt::receive_start("/Inbox");     // visible, accepts files into /Inbox
//   pd::bt::send_start(addr, {"/a.txt"}); // push files to a device
//   ... call pd::bt::poll() often, read status(), answer pairing requests ...
//   pd::bt::stop();
//
// Pairing uses numeric comparison: the same 6-digit code appears on both
// devices and the user confirms it (pair_request / pair_answer). Pairing
// keys are kept in a file on the SD card (set_key_file), never in flash.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pd::bt {

bool available();   // this build has Bluetooth

// Where pairing keys are stored (call before start()).
void set_key_file(const std::string& path);
// Name shown to other devices ("PicoCalc" -> "PicoCalc 12:34").
void set_device_name(const std::string& name);

bool start(std::string* err);
void stop();
bool powered();
void poll();                     // pump the stack; call from the main loop and while waiting
std::string address();           // "AA:BB:CC:DD:EE:FF" once powered

// ---------------------------------------------------------------- pairing
struct PairRequest {
    std::string device;          // name or address of the other device
    uint32_t code = 0;           // 6-digit code to compare (numeric comparison)
    bool just_works = false;     // no code to compare: just accept or not
};
bool pair_request(PairRequest* out);   // a request is waiting for an answer
void pair_answer(bool accept);

// ---------------------------------------------------------------- devices
struct Device {
    std::string addr;            // "AA:BB:CC:DD:EE:FF"
    std::string name;            // may be empty until resolved
    uint32_t cod = 0;            // class of device
    bool paired = false;
    const char* kind() const;    // "phone", "computer", ... from the class
};
bool scan_start();               // ~10 s inquiry; results in devices()
void scan_stop();
bool scanning();
std::string scan_error();        // why the last search stopped early ("" = it did not)
std::vector<Device> devices();   // paired + found, paired first
void forget(const std::string& addr);   // drop its pairing key

// ---------------------------------------------------------------- receive
struct Transfer {
    bool active = false;         // a file is being transferred now
    std::string device;          // other side
    std::string name;            // file name
    uint64_t done = 0, total = 0;   // bytes (total 0 = unknown)
};
struct ReceiveStatus {
    bool listening = false;      // visible and accepting
    bool connected = false;      // a device is connected
    Transfer current;
    std::vector<std::string> received;   // paths of complete files, oldest first
    std::string error;           // last problem, if any
};
bool receive_start(const std::string& dir);
void receive_stop();
ReceiveStatus receive_status();

// ---------------------------------------------------------------- send
enum class SendState { Idle, Connecting, Sending, Done, Failed, Cancelled };
struct SendStatus {
    SendState state = SendState::Idle;
    Transfer current;
    uint32_t file_index = 0, file_count = 0;
    uint32_t sent_ok = 0;
    std::string error;
};
bool send_start(const std::string& addr, const std::vector<std::string>& paths);
void send_cancel();
SendStatus send_status();

// ---------------------------------------------------------------- diagnostics
struct Diag {
    bool hci_on = false;
    uint32_t acl_packets = 0;    // ACL packets in + out
    uint32_t events = 0;         // HCI events
    uint8_t last_status = 0;     // last non-zero HCI / GOEP status
    std::string last_event;      // short text of the last notable event
    uint32_t inquiry_results = 0;   // devices reported by the last search
    uint16_t failed_opcode = 0;  // last HCI command the controller refused (0 = none)
    uint8_t failed_status = 0;   // ... and its status code
    uint32_t failed_count = 0;   // refused HCI commands since start
    bool discoverable = false;
    // Recent notable events, oldest first, "12.3 text" (seconds since start):
    // connections, pairing steps, encryption, refused commands, OBEX steps.
    std::vector<std::string> log;
    // Raw HCI event codes, most recent last ("03 31 32 33 36 18"), without
    // the frequent command-complete / packets-completed ones.
    std::string trail;
    // Every packet, from BTstack's hci_dump hook.
    uint32_t cmds_out = 0;       // HCI commands sent
    uint16_t last_cmd = 0;       // opcode of the last one
    uint32_t acl_in = 0, acl_out = 0;   // data packets received / sent
    bool sdp_idle = true;        // BTstack's SDP client is free (a send's first step needs it)
    int goep_state = -1;         // the send's GOEP client state (-1 none)
    // BTstack's own messages (errors and info), most recent last.
    std::vector<std::string> stack_log;
};
Diag diag();

}  // namespace pd::bt
