// pd_bt for the host simulator: two fake nearby devices; sending pretends to
// transfer; receiving plays a real OBEX Object Push session (built with
// BTstack's message builder) through the real ObexReceiver when
// PD_BT_SIM_FILE names a host file ("a phone sends it" ~1 s after receive
// mode starts, after a pairing prompt).
#include <cstdlib>
#include <cstring>

#include "obex_receiver.h"
#include "pd/bt.h"
#include "pd/sys.h"

extern "C" {
#include "btstack_util.h"
#include "classic/obex.h"
#include "classic/obex_message_builder.h"
}

namespace pd::bt {

namespace {
bool g_on = false;
bool g_listening = false;
uint32_t g_scan_until = 0;
detail::ObexReceiver g_rx;
std::vector<std::vector<uint8_t>> g_packets;   // simulated incoming requests
size_t g_pkt = 0, g_pkt_pos = 0;
uint32_t g_rx_start = 0;
bool g_paired = false, g_pair_pending = false;
PairRequest g_pair;
SendStatus g_send;
uint32_t g_send_t0 = 0;
bool g_send_cancel = false;
std::vector<std::string> g_paired_list;
Diag g_diag;

void build_session(const std::string& host_file) {
    g_packets.clear();
    FILE* f = fopen(host_file.c_str(), "rb");
    if (!f) return;
    std::vector<uint8_t> data;
    uint8_t b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) data.insert(data.end(), b, b + n);
    fclose(f);
    std::string name = host_file.substr(host_file.find_last_of('/') + 1);
    std::vector<uint8_t> p(1024);
    obex_message_builder_request_create_connect(p.data(), uint16_t(p.size()), OBEX_VERSION, 0, 1024);
    p.resize(big_endian_read_16(p.data(), 1));
    g_packets.push_back(p);
    size_t off = 0;
    bool first = true;
    do {
        p.assign(1024, 0);
        obex_message_builder_request_create_put(p.data(), 1024, OBEX_CONNECTION_ID_INVALID);
        if (first) {
            const std::vector<uint8_t> nm = detail::obex_name_utf16(name);
            obex_message_builder_header_add_variable(p.data(), 1024, OBEX_HEADER_NAME, nm.data(), uint16_t(nm.size()));
            obex_message_builder_header_add_length(p.data(), 1024, uint32_t(data.size()));
            first = false;
        }
        const uint16_t room = uint16_t(1024 - big_endian_read_16(p.data(), 1) - 3);
        const size_t k = std::min<size_t>(room, data.size() - off);
        const bool last = off + k == data.size();
        obex_message_builder_header_add_variable(p.data(), 1024, last ? OBEX_HEADER_END_OF_BODY : OBEX_HEADER_BODY,
                                                 data.data() + off, uint16_t(k));
        obex_message_builder_set_final_bit(p.data(), 1024, last);
        p.resize(big_endian_read_16(p.data(), 1));
        g_packets.push_back(p);
        off += k;
    } while (off < data.size());
    p.assign(16, 0);
    obex_message_builder_request_create_disconnect(p.data(), 16, OBEX_CONNECTION_ID_INVALID);
    p.resize(big_endian_read_16(p.data(), 1));
    g_packets.push_back(p);
}
}  // namespace

const char* Device::kind() const {
    switch ((cod >> 8) & 0x1F) {
        case 1: return "computer";
        case 2: return "phone";
        default: return "device";
    }
}

bool available() { return true; }
void set_key_file(const std::string&) {}
void set_device_name(const std::string&) {}
bool start(std::string*) {
    g_on = true;
    g_diag.hci_on = true;
    g_diag.log = {"0.0 radio ready"};
    g_diag.last_event = "radio ready";
    return true;
}
void stop() {
    receive_stop();
    g_on = false;
    g_diag.hci_on = false;
}
bool powered() { return g_on; }
std::string address() { return g_on ? "28:CD:C1:00:00:42" : ""; }

void poll() {
    const uint32_t now = sys::now_ms();
    // The phone starts pairing on its own once the search is over (PD_BT_SIM_PHONE_PAIR).
    static bool phone_paired_once = false;
    if (getenv("PD_BT_SIM_PHONE_PAIR") && !phone_paired_once && g_scan_until && sys::reached(g_scan_until + 400)) {
        phone_paired_once = true;
        g_pair = PairRequest{"Pixel 9a", 715034, false};
        g_pair_pending = true;
        fprintf(stderr, "[bt-sim] phone starts pairing\n");
    }
    // Simulated incoming session: pairing first, then a few KB per poll.
    if (g_listening && !g_packets.empty() && g_pkt < g_packets.size() && now - g_rx_start > 1000) {
        if (!g_paired) {
            if (!g_pair_pending) {
                g_pair = PairRequest{"Pixel 8", 482913, false};
                g_pair_pending = true;
            }
            return;
        }
        for (int budget = 4; budget > 0 && g_pkt < g_packets.size(); --budget) {
            const std::vector<uint8_t>& p = g_packets[g_pkt];
            const uint16_t k = uint16_t(std::min<size_t>(300, p.size() - g_pkt_pos));   // radio-sized chunks
            g_rx.feed(p.data() + g_pkt_pos, k);
            g_diag.acl_packets++;
            g_pkt_pos += k;
            if (g_pkt_pos == p.size()) {
                ++g_pkt;
                g_pkt_pos = 0;
            }
        }
    }
    // Simulated send: 2 s per file.
    if (g_send.state == SendState::Connecting && !getenv("PD_BT_SIM_STUCK") && now - g_send_t0 > 500)
        g_send.state = SendState::Sending;
    if (g_send.state == SendState::Sending) {
        if (g_send_cancel) {
            g_send.state = SendState::Cancelled;
        } else {
            const uint32_t per = 2000, t = now - g_send_t0 - 500;
            const uint32_t idx = t / per;
            if (idx >= g_send.file_count) {
                g_send.sent_ok = g_send.file_count;
                g_send.state = SendState::Done;
                g_send.current.active = false;
            } else {
                g_send.file_index = idx;
                g_send.current.active = true;
                g_send.current.total = 100000;
                g_send.current.done = uint64_t(t % per) * 100000 / per;
            }
        }
    }
}

bool pair_request(PairRequest* out) {
    if (!g_pair_pending) return false;
    if (out) *out = g_pair;
    return true;
}
void pair_answer(bool accept) {
    fprintf(stderr, "[bt-sim] pairing answered: %s\n", accept ? "yes" : "no");
    g_pair_pending = false;
    g_paired = accept;
    if (accept) g_paired_list.push_back("A4:C3:F0:12:34:56");
    else g_packets.clear();
}

bool scan_start() {
    g_scan_until = sys::now_ms() + 1500;
    return true;
}
void scan_stop() {
    if (!sys::reached(g_scan_until)) g_scan_until = sys::now_ms();   // end now, keep what was found
}
bool scanning() { return !sys::reached(g_scan_until); }
std::string scan_error() { return getenv("PD_BT_SIM_SCAN_FAIL") ? "the radio refused to search (status 0x0C)" : ""; }
std::vector<Device> devices() {
    std::vector<Device> d;
    if (getenv("PD_BT_SIM_SCAN_FAIL")) return d;
    const bool found = g_scan_until && sys::reached(g_scan_until);
    for (const std::string& a : g_paired_list) d.push_back(Device{a, "Pixel 8", 0x5a020c, true});
    if (found) {
        if (g_paired_list.empty()) d.push_back(Device{"A4:C3:F0:12:34:56", "Pixel 8", 0x5a020c, false});
        d.push_back(Device{"3C:22:FB:9A:BC:DE", "ettore-laptop", 0x6c010c, false});
    }
    return d;
}
void forget(const std::string&) { g_paired_list.clear(); }

bool receive_start(const std::string& dir) {
    g_rx.begin(dir);
    g_listening = true;
    g_diag.discoverable = true;
    g_pkt = g_pkt_pos = 0;
    g_rx_start = sys::now_ms();
    const char* f = getenv("PD_BT_SIM_FILE");
    if (f) build_session(f);
    return true;
}
void receive_stop() {
    g_listening = false;
    g_rx.closed();
}
ReceiveStatus receive_status() {
    ReceiveStatus s;
    s.listening = g_listening;
    s.connected = g_paired && g_pkt > 0 && g_pkt < g_packets.size();
    s.current.active = g_rx.active();
    s.current.device = "Pixel 8";
    s.current.name = g_rx.name();
    s.current.done = g_rx.done();
    s.current.total = g_rx.total();
    s.received = g_rx.received;
    s.error = g_rx.error;
    return s;
}

bool send_start(const std::string& addr, const std::vector<std::string>& paths) {
    g_diag.log.push_back("1.2 send: connecting to " + addr.substr(9));
    g_diag.last_event = "send: connecting to " + addr.substr(9);
    if (getenv("PD_BT_SIM_STUCK")) {   // stays "connecting", like the PicoCalc did
        g_diag.log.push_back("1.3 link to 4A:A5:93: ok 0x00");
        g_diag.log.push_back("1.4 pairing: capabilities asked");
        g_diag.trail = "18 08 6E 6E DD 01 6E 6E D8 0F 07 6E c0405 0F 03 a> a< c041B a> 0F ";
        g_diag.cmds_out = 57;
        g_diag.last_cmd = 0x041B;
        g_diag.acl_in = 3;
        g_diag.acl_out = 4;
        g_diag.sdp_idle = false;
        g_diag.goep_state = 1;
        g_diag.stack_log = {"L2CAP_CREATE_CHANNEL addr 34:39:16:4A:A5:93 psm 0x1", "create l2cap channel: state 3",
                            "l2cap_run: send info request"};
        g_diag.last_event = "pairing: capabilities asked";
        g_send = SendStatus();
        g_send.state = SendState::Connecting;
        g_send.file_count = uint32_t(paths.size());
        g_send.current.device = addr;
        g_send_t0 = UINT32_MAX / 2 + sys::now_ms();
        return true;
    }
    g_send = SendStatus();
    g_send.state = SendState::Connecting;
    g_send.file_count = uint32_t(paths.size());
    g_send.current.device = addr == "3C:22:FB:9A:BC:DE" ? "ettore-laptop" : "Pixel 8";
    g_send.current.name = paths.empty() ? "" : paths[0].substr(paths[0].find_last_of('/') + 1);
    g_send_t0 = sys::now_ms();
    g_send_cancel = false;
    return true;
}
void send_cancel() {
    g_send_cancel = true;
    if (g_send.state == SendState::Connecting) g_send.state = SendState::Cancelled;
}
SendStatus send_status() { return g_send; }
Diag diag() { return g_diag; }   // (the device fills sdp_idle / goep_state live)

}  // namespace pd::bt
