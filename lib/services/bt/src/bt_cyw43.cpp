// pd_bt on the Pico W / Pico 2 W: BTstack (Bluetooth Classic) on the CYW43.
// OBEX Object Push server (receive) and client (send) over RFCOMM, device
// discovery, pairing with numeric comparison.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

#include "btstack_modules.h"
#include "obex_receiver.h"
#include "pd/bt.h"
#include "pd/fs.h"
#include "pd/net.h"
#include "pd/path.h"
#include "pd/sys.h"

extern "C" {
#include "btstack.h"
#include "classic/goep_client.h"
#include "classic/goep_server.h"
#include "classic/obex.h"
#include "classic/obex_parser.h"
#include "classic/sdp_client.h"
#include "hci_dump.h"
}

namespace pd::bt {

namespace detail {
extern std::string g_key_file;
void save_keys_if_changed();
void on_stack_deinit();
}  // namespace detail

namespace {

constexpr uint8_t kOppChannel = 12;   // RFCOMM channel of our Object Push service

// ---------------------------------------------------------------- state
std::string g_name = "PicoCalc";
// gap_set_local_name() keeps the pointer (it sends the name later): it must
// stay valid, never a temporary.
std::string g_local_name;
uint32_t g_t0 = 0;
bool g_stack_ready = false;    // l2cap/rfcomm/sdp/goep set up (once per radio power-up)
bool g_powered = false;
bool g_holding = false;        // we hold the radio
btstack_packet_callback_registration_t g_hci_reg;
uint8_t g_sdp_record[160];
uint32_t g_sdp_handle = 0;
Diag g_diag;

// HCI Read Scan Enable (not predefined in BTstack): asks the radio chip
// whether it is visible (inquiry scan) and connectable (page scan).
const hci_cmd_t kReadScanEnable = {HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x19), ""};
uint32_t g_scan_check_at = 0;   // 0 = none planned
bool g_tx_wait_scan = false;    // send asked while a search runs: connect when it ends
uint32_t g_cmd_blocked_since = 0;   // when the controller stopped accepting commands
bool g_resetting = false;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    char b[72];
    const uint32_t t = sys::now_ms() - g_t0;
    int n = snprintf(b, sizeof b, "%lu.%lu ", (unsigned long)(t / 1000), (unsigned long)(t % 1000 / 100));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + n, sizeof b - size_t(n), fmt, ap);
    va_end(ap);
    g_diag.log.push_back(b);
    if (g_diag.log.size() > 14) g_diag.log.erase(g_diag.log.begin());
    g_diag.last_event = b + n;
}

std::string addr_str(const bd_addr_t a) { return bd_addr_to_str(a); }

// pairing
bool g_pair_pending = false;
bd_addr_t g_pair_addr;
PairRequest g_pair;
std::map<std::string, uint8_t> g_remote_io;   // address -> IO capability

// devices
std::vector<Device> g_found;
bool g_scanning = false;
uint32_t g_scan_deadline = 0;
std::string g_scan_error;
std::vector<std::string> g_name_queue;   // addresses to ask for names
std::map<std::string, std::pair<uint8_t, uint16_t>> g_page_info;
bool g_name_busy = false;
std::map<std::string, std::string> g_known_names;   // saved names of devices we used
std::string names_file() {
    const std::string& k = detail::g_key_file;
    return k.substr(0, k.find_last_of('/') + 1) + "bt_devices.txt";
}
void load_names() {
    g_known_names.clear();
    std::string t;
    if (!fs::read_file(names_file().c_str(), t)) return;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(p, e - p);
        if (line.size() > 18 && line[17] == '=') g_known_names[line.substr(0, 17)] = line.substr(18);
        p = e + 1;
    }
}
void remember_name(const std::string& addr, const std::string& name) {
    if (name.empty() || g_known_names[addr] == name) return;
    g_known_names[addr] = name;
    std::string t;
    for (const auto& kv : g_known_names) t += kv.first + "=" + kv.second + "\n";
    fs::write_file(names_file().c_str(), t, true);
}
std::string display_name(const std::string& addr) {
    auto it = g_known_names.find(addr);
    if (it != g_known_names.end()) return it->second;
    for (const Device& d : g_found)
        if (d.addr == addr && !d.name.empty()) return d.name;
    return addr;
}

// receive (server)
bool g_listening = false;
std::string g_recv_dir;
detail::ObexReceiver g_rx;
uint16_t g_rx_cid = 0;
std::string g_rx_device;
detail::ObexReceiver::Reply g_rx_reply = detail::ObexReceiver::Reply::None;
uint8_t g_rx_code = 0;

// send (client)
enum class Tx { Idle, W4Open, W4Retry, W2Connect, W4Connect, W2Put, W4Put, W2Disconnect, W4Disconnect, W4Closed };
Tx g_tx = Tx::Idle;
SendStatus g_send;
// One GOEP client object per attempt: an aborted one may still be linked in
// BTstack's lists, so it is never reused or freed while that can be the case.
goep_client_t* g_goep_client = nullptr;
uint16_t g_tx_cid = 0;
obex_parser_t g_tx_parser;
std::vector<std::string> g_tx_files;
fs::File g_tx_file;
bool g_tx_first_packet = true;
bool g_tx_cancel = false;
std::vector<uint8_t> g_tx_buf;
std::string g_tx_addr;            // target of the current send
bool g_tx_retried = false;        // already paired again once for this send
uint32_t g_tx_retry_at = 0;
std::string g_key_dropped_addr;   // device whose stale pairing key was just dropped
uint8_t g_tx_fail_status = 0;     // why the channel did not open
bool tx_connect();
void release_client_if_done();

void release_client_if_done() {
    // BTstack removes the client from its list on a failed open or a close:
    // only then is the object ours again.
    if (g_goep_client && g_goep_client->state == GOEP_CLIENT_INIT) {
        free(g_goep_client);
        g_goep_client = nullptr;
    }
}

void finish_send(SendState st, const std::string& err = std::string()) {
    release_client_if_done();
    g_tx_file.close();
    g_send.state = st;
    if (!err.empty()) g_send.error = err;
    g_send.current.active = false;
    g_tx = Tx::Idle;
    g_tx_cid = 0;
}

// ---------------------------------------------------------------- SDP
void register_sdp() {
    if (g_sdp_handle) return;
    g_sdp_handle = sdp_create_service_record_handle();
    detail::build_opp_sdp_record(g_sdp_record, sizeof g_sdp_record, g_sdp_handle, kOppChannel);
    sdp_register_service(g_sdp_record);
}

// ---------------------------------------------------------------- scanning
void next_name_request() {
    if (g_name_busy || g_scanning || g_name_queue.empty()) return;
    const std::string a = g_name_queue.front();
    g_name_queue.erase(g_name_queue.begin());
    bd_addr_t addr;
    if (!sscanf_bd_addr(a.c_str(), addr)) return;
    const auto pi = g_page_info[a];
    if (gap_remote_name_request(addr, pi.first, pi.second | 0x8000) == 0) g_name_busy = true;
}

Device* found(const std::string& a) {
    for (Device& d : g_found)
        if (d.addr == a) return &d;
    return nullptr;
}

// ---------------------------------------------------------------- server
void server_handler(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if (type == GOEP_DATA_PACKET) {
        ++g_diag.acl_packets;
        if (channel != g_rx_cid) return;
        const auto r = g_rx.feed(packet, size);
        if (r != detail::ObexReceiver::Reply::None) {
            g_rx_reply = r;
            g_rx_code = g_rx.response_code();
            const uint8_t op = g_rx.last_opcode();
            const char* what = op == 0x80 ? "CONNECT" : op == 0x81 ? "DISCONNECT" : (op & 0x7F) == 0x02 ? (op & 0x80 ? "PUT end" : "PUT") :
                               op == 0xFF ? "ABORT" : (op & 0x7F) == 0x03 ? "GET" : op == 0x85 ? "SETPATH" : "request";
            // PUT parts with CONTINUE would flood the log: only the first one
            static uint32_t continues = 0;
            if (r == detail::ObexReceiver::Reply::General && g_rx_code == 0x90) {
                if (continues++ == 0) logf("receive: PUT \"%s\" -> continue", g_rx.name().c_str());
            } else {
                continues = 0;
                logf("receive: %s 0x%02X -> 0x%02X%s", what, op, r == detail::ObexReceiver::Reply::Connect ? 0xA0 : g_rx_code,
                     g_rx_code >= 0xC0 ? " REFUSED" : "");
                if (!g_rx.error.empty()) logf("receive: %s", g_rx.error.c_str());
            }
            goep_server_request_can_send_now(g_rx_cid);
        }
        return;
    }
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GOEP_META) return;
    switch (hci_event_goep_meta_get_subevent_code(packet)) {
        case GOEP_SUBEVENT_INCOMING_CONNECTION: {
            const uint16_t cid = goep_subevent_incoming_connection_get_goep_cid(packet);
            if (g_listening && g_rx_cid == 0) {
                bd_addr_t a;
                goep_subevent_incoming_connection_get_address(packet, a);
                g_rx_device = display_name(addr_str(a));
                goep_server_accept_connection(cid);
                logf("receive: connection accepted");
            } else {
                goep_server_decline_connection(cid);
                logf("receive: connection declined (%s)", g_listening ? "already busy" : "not in receive mode");
            }
            break;
        }
        case GOEP_SUBEVENT_CONNECTION_OPENED:
            if (goep_subevent_connection_opened_get_status(packet)) {
                g_diag.last_status = goep_subevent_connection_opened_get_status(packet);
                logf("receive: open FAILED 0x%02X", g_diag.last_status);
                break;
            }
            g_rx_cid = goep_subevent_connection_opened_get_goep_cid(packet);
            g_rx_reply = detail::ObexReceiver::Reply::None;
            logf("receive: connected (frame %u)", goep_server_response_get_max_message_size(g_rx_cid));
            break;
        case GOEP_SUBEVENT_CONNECTION_CLOSED:
            if (goep_subevent_connection_closed_get_goep_cid(packet) == g_rx_cid) {
                g_rx.closed();
                g_rx_cid = 0;
                logf("receive: closed");
            }
            break;
        case GOEP_SUBEVENT_CAN_SEND_NOW: {
            const uint16_t cid = goep_subevent_can_send_now_get_goep_cid(packet);
            if (cid != g_rx_cid) break;
            if (g_rx_reply == detail::ObexReceiver::Reply::Connect) {
                goep_server_response_create_connect(cid, OBEX_VERSION, 0, goep_server_response_get_max_message_size(cid));
                goep_server_execute(cid, OBEX_RESP_SUCCESS);
            } else if (g_rx_reply == detail::ObexReceiver::Reply::General) {
                goep_server_response_create_general(cid);
                goep_server_execute(cid, g_rx_code);
            }
            g_rx_reply = detail::ObexReceiver::Reply::None;
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- client
void tx_parser_cb(void* user, uint8_t id, uint16_t total_len, uint16_t offset, const uint8_t* data, uint16_t len) {
    (void)user;
    static uint8_t cid_buf[4];
    if (id == OBEX_HEADER_CONNECTION_ID &&
        obex_parser_header_store(cid_buf, 4, total_len, offset, data, len) == OBEX_PARSER_HEADER_COMPLETE)
        goep_client_set_connection_id(g_tx_cid, big_endian_read_32(cid_buf, 0));
}

bool open_next_file() {
    g_tx_file.close();
    while (g_send.file_index < g_send.file_count) {
        const std::string& p = g_tx_files[g_send.file_index];
        fs::Info in;
        if (fs::stat(p.c_str(), &in) && !in.is_dir && g_tx_file.open(p, fs::Mode::Read)) {
            g_send.current = Transfer{true, g_send.current.device, pd::path::base_name(p), 0, in.size};
            g_tx_first_packet = true;
            return true;
        }
        ++g_send.file_index;   // skip what cannot be read
    }
    return false;
}

void tx_can_send() {
    const uint16_t cid = g_tx_cid;
    switch (g_tx) {
        case Tx::W2Connect:
            goep_client_request_create_connect(cid, OBEX_VERSION, 0, OBEX_MAX_PACKETLEN_DEFAULT);
            obex_parser_init_for_response(&g_tx_parser, OBEX_OPCODE_CONNECT, tx_parser_cb, nullptr);
            g_tx = Tx::W4Connect;
            goep_client_execute(cid);
            break;
        case Tx::W2Put: {
            goep_client_request_create_put(cid);
            if (g_tx_first_packet) {
                // Proper UTF-16 (goep_client_header_add_name garbles non-ASCII names).
                const std::vector<uint8_t> nm = detail::obex_name_utf16(g_send.current.name);
                goep_client_header_add_variable(cid, OBEX_HEADER_NAME, nm.data(), uint16_t(nm.size()));
                goep_client_header_add_length(cid, uint32_t(g_send.current.total));
                g_tx_first_packet = false;
            }
            const uint16_t room = goep_client_request_get_max_body_size(cid);
            const uint16_t chunk_max = room > 3 ? uint16_t(room - 3) : 0;   // 3 bytes of header
            const uint64_t left = g_send.current.total - g_send.current.done;
            const uint16_t n = uint16_t(std::min<uint64_t>(left, chunk_max));
            g_tx_buf.resize(n ? n : 1);
            const int r = n ? g_tx_file.read(g_tx_buf.data(), n) : 0;
            if (r != int(n)) {
                goep_client_request_create_abort(cid);
                obex_parser_init_for_response(&g_tx_parser, OBEX_OPCODE_ABORT, nullptr, nullptr);
                g_send.error = "cannot read " + g_send.current.name;
                g_tx = Tx::W2Disconnect;
                goep_client_execute(cid);
                break;
            }
            const bool last = uint64_t(n) == left;
            goep_client_header_add_variable(cid, last ? OBEX_HEADER_END_OF_BODY : OBEX_HEADER_BODY, g_tx_buf.data(), n);
            g_send.current.done += n;
            obex_parser_init_for_response(&g_tx_parser, OBEX_OPCODE_PUT, tx_parser_cb, nullptr);
            g_tx = Tx::W4Put;
            goep_client_execute_with_final_bit(cid, last);
            break;
        }
        case Tx::W2Disconnect:
            goep_client_request_create_disconnect(cid);
            obex_parser_init_for_response(&g_tx_parser, OBEX_OPCODE_DISCONNECT, nullptr, nullptr);
            g_tx = Tx::W4Disconnect;
            goep_client_execute(cid);
            break;
        default:
            break;
    }
}

void tx_response(uint8_t code) {
    if (g_tx == Tx::W4Connect) logf("send: OBEX connect answer 0x%02X", code);
    else if (g_tx == Tx::W4Put && code != OBEX_RESP_CONTINUE) logf("send: file answer 0x%02X", code);
    switch (g_tx) {
        case Tx::W4Connect:
            if (code != OBEX_RESP_SUCCESS) {
                goep_client_disconnect(g_tx_cid);
                g_send.error = "the device refused the connection";
                g_tx = Tx::W4Closed;
                return;
            }
            if (!open_next_file()) {
                g_send.error = "nothing to send";
                g_tx = Tx::W2Disconnect;
            } else {
                g_send.state = SendState::Sending;
                g_tx = Tx::W2Put;
            }
            goep_client_request_can_send_now(g_tx_cid);
            break;
        case Tx::W4Put:
            if (g_tx_cancel) {
                g_tx = Tx::W2Disconnect;
            } else if (code == OBEX_RESP_CONTINUE) {
                g_tx = Tx::W2Put;
            } else if (code == OBEX_RESP_SUCCESS) {
                ++g_send.sent_ok;
                remember_name(g_send.current.device, display_name(g_send.current.device));
                ++g_send.file_index;
                g_tx = open_next_file() ? Tx::W2Put : Tx::W2Disconnect;
            } else {
                char b[80];
                snprintf(b, sizeof b, "the device declined %s (code 0x%02X)", g_send.current.name.c_str(), code);
                g_send.error = b;
                g_tx = Tx::W2Disconnect;
            }
            goep_client_request_can_send_now(g_tx_cid);
            break;
        case Tx::W4Disconnect:
            goep_client_disconnect(g_tx_cid);
            g_tx = Tx::W4Closed;
            break;
        default:
            break;
    }
}

void client_handler(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size) {
    if (g_tx == Tx::Idle) {   // a stopped send: tidy up whatever still arrives for it
        if (type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == HCI_EVENT_GOEP_META &&
            hci_event_goep_meta_get_subevent_code(packet) == GOEP_SUBEVENT_CONNECTION_OPENED &&
            goep_subevent_connection_opened_get_status(packet) == 0)
            goep_client_disconnect(goep_subevent_connection_opened_get_goep_cid(packet));
        return;
    }
    if (type == GOEP_DATA_PACKET) {
        ++g_diag.acl_packets;
        if (channel != g_tx_cid) return;
        if (obex_parser_process_data(&g_tx_parser, packet, size) == OBEX_PARSER_OBJECT_STATE_COMPLETE) {
            obex_parser_operation_info_t op;
            obex_parser_get_operation_info(&g_tx_parser, &op);
            tx_response(op.response_code);
        }
        return;
    }
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GOEP_META) return;
    switch (hci_event_goep_meta_get_subevent_code(packet)) {
        case GOEP_SUBEVENT_CONNECTION_OPENED: {
            const uint8_t st = goep_subevent_connection_opened_get_status(packet);
            logf("send: channel %s 0x%02X", st ? "FAILED" : "open", st);
            if (st && !g_tx_cancel && !g_tx_retried &&
                (st == L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY || st == ERROR_CODE_AUTHENTICATION_FAILURE ||
                 st == ERROR_CODE_PIN_OR_KEY_MISSING || st == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS)) {
                // Security failures: the authentication result may arrive just
                // after this event. "Connection already exists": the other
                // device is connecting to us at the same moment (pairing from
                // its side). Decide in a moment (poll()) whether to try again.
                g_tx_fail_status = st;
                g_tx = Tx::W4Retry;
                g_tx_retry_at = sys::now_ms() + (st == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS ? 2500 : 1500);
                return;
            }
            if (st) {
                g_diag.last_status = st;
                char b[96];
                snprintf(b, sizeof b, "could not connect (0x%02X): is the device on, in range, and able to receive files?", st);
                finish_send(g_tx_cancel ? SendState::Cancelled : SendState::Failed, g_tx_cancel ? "" : b);
                return;
            }
            g_tx = Tx::W2Connect;
            if (g_tx_cancel) g_tx = Tx::W2Disconnect;
            goep_client_request_can_send_now(g_tx_cid);
            break;
        }
        case GOEP_SUBEVENT_CAN_SEND_NOW:
            tx_can_send();
            break;
        case GOEP_SUBEVENT_CONNECTION_CLOSED:
            logf("send: channel closed");
            if (g_tx_cancel) finish_send(SendState::Cancelled);
            else if (g_send.error.empty() && g_send.sent_ok == g_send.file_count && g_send.file_count)
                finish_send(SendState::Done);
            else if (g_send.error.empty()) finish_send(SendState::Failed, "connection lost");
            else finish_send(g_send.sent_ok ? SendState::Done : SendState::Failed);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- HCI events
void hci_handler(uint8_t type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    ++g_diag.events;
    {
        const uint8_t code = hci_event_packet_get_type(packet);
        if (code == HCI_EVENT_COMMAND_COMPLETE) {
            const uint16_t op = hci_event_command_complete_get_command_opcode(packet);
            if (op != HCI_OPCODE_HCI_HOST_NUMBER_OF_COMPLETED_PACKETS) {
                char h[8];
                snprintf(h, sizeof h, "k%04X ", op);
                g_diag.trail += h;
            }
        } else if (code != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS) {
            char h[4];
            snprintf(h, sizeof h, "%02X ", code);
            g_diag.trail += h;
            if (g_diag.trail.size() > 90) g_diag.trail.erase(0, g_diag.trail.size() - 90);
        }
    }
    bd_addr_t a;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
        {
            const uint8_t hs = btstack_event_state_get_state(packet);
            g_powered = hs == HCI_STATE_WORKING;
            if (g_powered && g_listening) {
                gap_connectable_control(1);
                gap_discoverable_control(1);
            }
            g_diag.hci_on = g_powered;
            logf("%s", hs == HCI_STATE_WORKING        ? "radio ready"
                       : hs == HCI_STATE_OFF          ? "radio off"
                       : hs == HCI_STATE_INITIALIZING ? "radio starting"
                       : hs == HCI_STATE_HALTING      ? "radio stopping"
                                                      : "radio sleeping");
        }
            break;
        case HCI_EVENT_CONNECTION_REQUEST:
            hci_event_connection_request_get_bd_addr(packet, a);
            logf("link request from %s", addr_str(a).c_str() + 9);
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            hci_event_connection_complete_get_bd_addr(packet, a);
            logf("link to %s: %s 0x%02X", addr_str(a).c_str() + 9,
                 hci_event_connection_complete_get_status(packet) ? "FAILED" : "ok",
                 hci_event_connection_complete_get_status(packet));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            logf("link closed, reason 0x%02X", hci_event_disconnection_complete_get_reason(packet));
            break;
        case HCI_EVENT_IO_CAPABILITY_REQUEST:
            logf("pairing: capabilities asked");
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            const uint8_t st = hci_event_authentication_complete_get_status(packet);
            logf("authentication: %s 0x%02X", st ? "FAILED" : "ok", st);
            // 0x06 PIN or Key Missing / 0x05 Authentication Failure: the other
            // device no longer has the pairing we stored (it was removed there,
            // or the pairing never completed on its side). Drop ours, so the
            // next attempt pairs afresh instead of failing the same way.
            if (st == ERROR_CODE_PIN_OR_KEY_MISSING || st == ERROR_CODE_AUTHENTICATION_FAILURE) {
                const hci_connection_t* c =
                    hci_connection_for_handle(hci_event_authentication_complete_get_connection_handle(packet));
                if (c) {
                    bd_addr_t ka;
                    memcpy(ka, c->address, sizeof(bd_addr_t));
                    gap_drop_link_key_for_bd_addr(ka);
                    g_key_dropped_addr = addr_str(ka);
                    logf("old pairing dropped, will pair again");
                }
            }
            break;
        }
        case HCI_EVENT_ENCRYPTION_CHANGE:
            logf("encryption %s, status 0x%02X", hci_event_encryption_change_get_encryption_enabled(packet) ? "on" : "off",
                 hci_event_encryption_change_get_status(packet));
            break;
        case HCI_EVENT_LINK_KEY_REQUEST:
            logf("link key asked");
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            logf("link key stored");
            break;
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t st = hci_event_command_status_get_status(packet);
            const uint16_t op = hci_event_command_status_get_command_opcode(packet);
            if (st && op != HCI_OPCODE_HCI_INQUIRY_CANCEL) logf("cmd 0x%04X refused 0x%02X", op, st);
            if (st && op != HCI_OPCODE_HCI_INQUIRY_CANCEL) {
                g_diag.failed_opcode = op;
                g_diag.failed_status = st;
                ++g_diag.failed_count;
                // BTstack resets its inquiry state on a refused Inquiry but
                // sends no INQUIRY_COMPLETE: end the search here.
                if (op == HCI_OPCODE_HCI_INQUIRY && g_scanning) {
                    g_scanning = false;
                    char b[64];
                    snprintf(b, sizeof b, "the radio refused to search (status 0x%02X)", st);
                    g_scan_error = b;
                }
            }
            break;
        }
        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t* rp = hci_event_command_complete_get_return_parameters(packet);
            const uint16_t op = hci_event_command_complete_get_command_opcode(packet);
            if (op == kReadScanEnable.opcode && rp && rp[0] == 0)
                logf("radio listening: %s, %s", (rp[1] & 1) ? "visible" : "HIDDEN",
                     (rp[1] & 2) ? "connectable" : "NOT CONNECTABLE");
            if (rp && rp[0] && op != HCI_OPCODE_HCI_INQUIRY_CANCEL) {
                logf("cmd 0x%04X failed 0x%02X", op, rp[0]);
                g_diag.failed_opcode = hci_event_command_complete_get_command_opcode(packet);
                g_diag.failed_status = rp[0];
                ++g_diag.failed_count;
            }
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST:   // legacy device: fixed PIN
            hci_event_pin_code_request_get_bd_addr(packet, a);
            gap_pin_code_response(a, "0000");
            break;
        case HCI_EVENT_IO_CAPABILITY_RESPONSE:
            hci_event_io_capability_response_get_bd_addr(packet, a);
            g_remote_io[addr_str(a)] = hci_event_io_capability_response_get_io_capability(packet);
            logf("pairing: other side capability %u", hci_event_io_capability_response_get_io_capability(packet));
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            hci_event_user_confirmation_request_get_bd_addr(packet, a);
            const std::string s = addr_str(a);
            memcpy(g_pair_addr, a, sizeof(bd_addr_t));
            g_pair.device = display_name(s);
            g_pair.code = hci_event_user_confirmation_request_get_numeric_value(packet);
            g_pair.just_works = g_remote_io.count(s) && g_remote_io[s] == SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
            g_pair_pending = true;
            logf("pairing: code %06lu shown", (unsigned long)g_pair.code);
            break;
        }
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            g_diag.last_status = hci_event_simple_pairing_complete_get_status(packet);
            logf("pairing: %s 0x%02X", g_diag.last_status ? "FAILED" : "done", g_diag.last_status);
            if (!g_diag.last_status) {
                hci_event_simple_pairing_complete_get_bd_addr(packet, a);
                const std::string s = addr_str(a);
                remember_name(s, display_name(s) == s ? std::string() : display_name(s));
            }
            g_pair_pending = false;
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            ++g_diag.inquiry_results;
            gap_event_inquiry_result_get_bd_addr(packet, a);
            const std::string s = addr_str(a);
            Device* d = found(s);
            if (!d) {
                g_found.push_back(Device{s, "", 0, false});
                d = &g_found.back();
            }
            d->cod = gap_event_inquiry_result_get_class_of_device(packet);
            g_page_info[s] = {gap_event_inquiry_result_get_page_scan_repetition_mode(packet),
                              gap_event_inquiry_result_get_clock_offset(packet)};
            if (gap_event_inquiry_result_get_name_available(packet)) {
                const int n = gap_event_inquiry_result_get_name_len(packet);
                d->name.assign(reinterpret_cast<const char*>(gap_event_inquiry_result_get_name(packet)), size_t(n));
            } else if (d->name.empty() && std::find(g_name_queue.begin(), g_name_queue.end(), s) == g_name_queue.end()) {
                g_name_queue.push_back(s);
            }
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            logf("search done, %lu found", (unsigned long)g_diag.inquiry_results);
            g_scanning = false;
            next_name_request();
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            g_name_busy = false;
            hci_event_remote_name_request_complete_get_bd_addr(packet, a);
            if (hci_event_remote_name_request_complete_get_status(packet) == 0) {
                const std::string s = addr_str(a);
                const char* n = hci_event_remote_name_request_complete_get_remote_name(packet);
                if (Device* d = found(s)) d->name = n;
                if (g_known_names.count(s)) remember_name(s, n);
            }
            next_name_request();
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- packet log
void dump_reset() {}
void dump_packet(uint8_t type, uint8_t in, uint8_t* packet, uint16_t len) {
    (void)len;
    if (type == HCI_COMMAND_DATA_PACKET && !in) {
        ++g_diag.cmds_out;
        g_diag.last_cmd = little_endian_read_16(packet, 0);
        if (g_diag.last_cmd != HCI_OPCODE_HCI_HOST_NUMBER_OF_COMPLETED_PACKETS) {
            char h[8];
            snprintf(h, sizeof h, "c%04X ", g_diag.last_cmd);
            g_diag.trail += h;
        }
    } else if (type == HCI_ACL_DATA_PACKET) {
        if (in) ++g_diag.acl_in;
        else ++g_diag.acl_out;
        g_diag.trail += in ? "a< " : "a> ";
    } else {
        return;   // events are added by the event handler
    }
    if (g_diag.trail.size() > 90) g_diag.trail.erase(0, g_diag.trail.size() - 90);
}
void dump_message(int level, const char* fmt, va_list ap) {
    char b[96];
    vsnprintf(b, sizeof b, fmt, ap);
    g_diag.stack_log.push_back(std::string(level == HCI_DUMP_LOG_LEVEL_ERROR ? "E " : "") + b);
    if (g_diag.stack_log.size() > 12) g_diag.stack_log.erase(g_diag.stack_log.begin());
}
const hci_dump_t kDump = {dump_reset, dump_packet, dump_message};

void setup_stack() {
    if (g_stack_ready) return;
    hci_dump_init(&kDump);
    l2cap_init();
    sdp_init();
    rfcomm_init();
    goep_server_init();
    goep_client_init();
    g_local_name = g_name + " 00:00:00:00:00:00";   // BTstack puts the address in place of the zeros
    gap_set_local_name(g_local_name.c_str());
    gap_set_class_of_device(0x100110);   // Object Transfer service; computer, handheld
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_auto_accept(0);
    gap_set_bondable_mode(1);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    g_hci_reg.callback = &hci_handler;
    hci_add_event_handler(&g_hci_reg);
    register_sdp();
    goep_server_register_service(&server_handler, kOppChannel, 0xFFFF, 0, 0, LEVEL_2);
    g_stack_ready = true;
}

}  // namespace

const char* Device::kind() const {
    switch ((cod >> 8) & 0x1F) {   // major device class
        case 1: return "computer";
        case 2: return "phone";
        case 3: return "network";
        case 4: return "audio";
        case 5: return "input";
        case 6: return "imaging";
        default: return "device";
    }
}

bool available() { return true; }
void set_key_file(const std::string& path) { detail::g_key_file = path; }
void set_device_name(const std::string& name) { g_name = name; }

bool start(std::string* err) {
    if (g_powered && g_holding) {
        gap_connectable_control(1);   // back from idle
        logf("bluetooth active");
        return true;
    }
    if (!g_holding) {
        if (!net::radio_acquire(err)) return false;
        g_holding = true;
    }
    if (!g_t0) g_t0 = sys::now_ms();
    load_names();
    setup_stack();
    hci_power_control(HCI_POWER_ON);
    const uint32_t until = sys::now_ms() + 5000;
    while (!g_powered && !sys::reached(until)) {
        net::poll();
        sys::sleep_ms(2);
    }
    if (!g_powered) {
        if (err) *err = "Bluetooth did not start";
        return false;
    }
    return true;
}

void stop() {
    // Bluetooth stays up for the rest of the session once started: the
    // CYW43's Bluetooth side does not come back reliably after the radio is
    // shut down and started again (commands still answered, but no paging /
    // page scan: transfers worked once, then never again). So "stop" only
    // makes the PicoCalc invisible and not connectable, ends transfers and
    // saves the keys; start() re-enables it instantly.
    receive_stop();
    if (g_tx != Tx::Idle) send_cancel();
    scan_stop();
    if (g_powered) {
        gap_discoverable_control(0);
        gap_connectable_control(0);
        g_diag.discoverable = false;
        logf("bluetooth idle (hidden, not connectable)");
    }
    const uint32_t until = sys::now_ms() + 100;
    while (!sys::reached(until)) net::poll();
    detail::save_keys_if_changed();
}

bool powered() { return g_powered; }

void poll() {
    net::poll();
    if (g_tx_wait_scan && (!g_scanning || g_tx == Tx::Idle)) {
        g_tx_wait_scan = false;
        if (g_tx != Tx::Idle && !g_tx_cancel && !tx_connect()) finish_send(SendState::Failed, "could not start the connection");
    }
    // Watchdog: the controller must answer every command. If it stops
    // (nothing can be sent for 4 s), BTstack waits forever and the radio is
    // deaf: reset the Bluetooth controller.
    if (g_powered && !g_resetting) {
        if (hci_can_send_command_packet_now()) {
            g_cmd_blocked_since = 0;
        } else if (!g_cmd_blocked_since) {
            g_cmd_blocked_since = sys::now_ms() | 1;
        } else if (sys::now_ms() - g_cmd_blocked_since > 4000) {
            logf("radio stuck: command %04X unanswered, resetting Bluetooth", g_diag.last_cmd);
            g_resetting = true;
            g_cmd_blocked_since = 0;
            if (g_tx != Tx::Idle) finish_send(SendState::Failed, "the radio stopped answering (it was reset, please try again)");
            g_scanning = false;
            hci_power_control(HCI_POWER_OFF);
        }
    }
    if (g_resetting && !g_powered && hci_get_state() == HCI_STATE_OFF) {
        g_resetting = false;
        hci_power_control(HCI_POWER_ON);
        if (g_listening) g_scan_check_at = sys::now_ms() + 2000;
    }
    // Check what the radio chip really does while receiving.
    if (g_scan_check_at && g_powered && sys::reached(g_scan_check_at) && hci_can_send_command_packet_now()) {
        hci_send_cmd(&kReadScanEnable);
        g_scan_check_at = g_listening ? sys::now_ms() + 10000 : 0;
    }
    if (g_tx == Tx::W4Retry && sys::reached(g_tx_retry_at)) {
        if (g_tx_cancel) {
            finish_send(SendState::Cancelled);
        } else if (g_key_dropped_addr == g_tx_addr || g_tx_fail_status == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS) {
            g_tx_retried = true;
            logf("send: %s", g_key_dropped_addr == g_tx_addr ? "pairing again" : "trying again over the existing link");
            if (!tx_connect()) finish_send(SendState::Failed, "could not start the connection");
        } else {
            finish_send(SendState::Failed, "the device refused the secure connection (pairing failed)");
        }
    }
    detail::save_keys_if_changed();
}

std::string address() {
    if (!g_powered) return "";
    bd_addr_t a;
    gap_local_bd_addr(a);
    return addr_str(a);
}

bool pair_request(PairRequest* out) {
    if (!g_pair_pending) return false;
    if (out) *out = g_pair;
    return true;
}

void pair_answer(bool accept) {
    if (!g_pair_pending) return;
    g_pair_pending = false;
    logf("pairing: you answered %s", accept ? "yes" : "no");
    if (accept) gap_ssp_confirmation_response(g_pair_addr);
    else gap_ssp_confirmation_negative(g_pair_addr);
}

bool scan_start() {
    g_scan_error.clear();
    if (!g_powered) {
        g_scan_error = "Bluetooth is off";
        return false;
    }
    g_found.clear();
    g_name_queue.clear();
    g_diag.inquiry_results = 0;
    // Visible while searching too, so the other side can find us as well.
    gap_connectable_control(1);
    gap_discoverable_control(1);
    g_diag.discoverable = true;
    const int rc = gap_inquiry_start(6);   // 6 x 1.28 s, never cancelled (see scan_stop)
    g_scanning = rc == 0;
    if (!g_scanning) {
        char b[48];
        snprintf(b, sizeof b, "search could not start (0x%02X)", unsigned(rc));
        g_scan_error = b;
    }
    g_scan_deadline = sys::now_ms() + 15000;   // the controller should finish in ~8 s
    return g_scanning;
}

void scan_stop() {
    // The search is left to finish on its own: the CYW43 has refused or
    // ignored "Inquiry Cancel", and an unanswered command blocks every
    // later one (no connection could be made afterwards).
    g_name_queue.clear();
    if (!g_listening) {
        gap_discoverable_control(0);
        g_diag.discoverable = false;
    }
}

bool scanning() {
    if (g_scanning && sys::reached(g_scan_deadline)) {
        g_scanning = false;
        if (g_found.empty()) g_scan_error = "the search did not finish (no answer from the radio)";
    }
    if (!g_scanning && g_name_busy && sys::reached(g_scan_deadline + 10000)) g_name_busy = false;   // a stuck name request
    return g_scanning || g_name_busy || !g_name_queue.empty();
}

std::string scan_error() { return g_scan_error; }

std::vector<Device> devices() {
    std::vector<Device> out;
    btstack_link_key_iterator_t it;
    if (g_powered && gap_link_key_iterator_init(&it)) {
        bd_addr_t a;
        link_key_t k;
        link_key_type_t t;
        while (gap_link_key_iterator_get_next(&it, a, k, &t)) {
            const std::string s = addr_str(a);
            Device d{s, display_name(s) == s ? "" : display_name(s), 0, true};
            if (const Device* f = found(s)) d.cod = f->cod;
            out.push_back(d);
        }
        gap_link_key_iterator_done(&it);
    }
    for (const Device& f : g_found) {
        bool dup = false;
        for (const Device& d : out) dup = dup || d.addr == f.addr;
        if (!dup) out.push_back(f);
    }
    return out;
}

void forget(const std::string& addr) {
    bd_addr_t a;
    if (sscanf_bd_addr(addr.c_str(), a)) gap_drop_link_key_for_bd_addr(a);
}

bool receive_start(const std::string& dir) {
    if (!g_powered) return false;
    g_recv_dir = dir;
    g_rx.begin(dir);
    g_listening = true;
    gap_discoverable_control(1);
    gap_connectable_control(1);
    g_diag.discoverable = true;
    g_scan_check_at = sys::now_ms() + 1000;
    logf("receive: listening");
    return true;
}

void receive_stop() {
    if (!g_listening) return;
    g_listening = false;
    if (g_powered) gap_discoverable_control(0);
    g_diag.discoverable = false;
    if (g_rx_cid) goep_server_disconnect(g_rx_cid);
    g_rx.closed();
}

ReceiveStatus receive_status() {
    ReceiveStatus s;
    s.listening = g_listening;
    s.connected = g_rx_cid != 0;
    s.current.active = g_rx.active();
    s.current.device = g_rx_device;
    s.current.name = g_rx.name();
    s.current.done = g_rx.done();
    s.current.total = g_rx.total();
    s.received = g_rx.received;
    s.error = g_rx.error;
    return s;
}

namespace {
bool tx_connect() {
    bd_addr_t a;
    if (!sscanf_bd_addr(g_tx_addr.c_str(), a)) return false;
    release_client_if_done();
    g_goep_client = nullptr;   // one still in use by BTstack is left alone
    g_goep_client = static_cast<goep_client_t*>(calloc(1, sizeof(goep_client_t)));
    if (!g_goep_client) return false;
    g_tx = Tx::W4Open;
    logf("send: connecting to %s", g_tx_addr.c_str() + 9);
    const uint8_t st = goep_client_connect(g_goep_client, nullptr, nullptr, 0, &client_handler, a,
                                           BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH, 0, &g_tx_cid);
    if (st) {
        g_diag.last_status = st;
        logf("send: connect call FAILED 0x%02X", st);
        return false;
    }
    return true;
}
}  // namespace

bool send_start(const std::string& addr, const std::vector<std::string>& paths) {
    if (!g_powered || g_tx != Tx::Idle || paths.empty()) return false;
    bd_addr_t a;
    if (!sscanf_bd_addr(addr.c_str(), a)) return false;
    g_send = SendStatus();
    g_send.state = SendState::Connecting;
    g_send.file_count = uint32_t(paths.size());
    g_send.current.device = addr;
    g_tx_files = paths;
    g_tx_cancel = false;
    g_tx_addr = addr;
    g_tx_retried = false;
    g_key_dropped_addr.clear();
    g_tx_fail_status = 0;
    (void)a;
    if (g_scanning) {   // the search must end first (it is never cancelled)
        g_tx_wait_scan = true;
        g_tx = Tx::W4Open;
        logf("send: waiting for the search to end");
        return true;
    }
    if (!tx_connect()) {
        finish_send(SendState::Failed, "could not start the connection");
        return false;
    }
    return true;
}

void send_cancel() {
    if (g_tx == Tx::Idle) return;
    g_tx_cancel = true;
    logf("send: stopped by you");
    // Stop now, whatever the phase: close the OBEX channel if there is one
    // and drop the radio link, so the next attempt starts clean.
    g_tx_wait_scan = false;
    if (g_tx_cid) goep_client_disconnect(g_tx_cid);
    bd_addr_t a;
    if (sscanf_bd_addr(g_tx_addr.c_str(), a)) {
        if (const hci_connection_t* c = hci_connection_for_bd_addr_and_type(a, BD_ADDR_TYPE_ACL)) gap_disconnect(c->con_handle);
    }
    finish_send(SendState::Cancelled);
    g_goep_client = nullptr;   // may still be in BTstack's lists: leave it there (small, rare)
}

SendStatus send_status() {
    SendStatus s = g_send;
    if (!s.current.device.empty()) s.current.device = display_name(s.current.device);
    return s;
}

Diag diag() {
    Diag d = g_diag;
    d.sdp_idle = g_stack_ready ? sdp_client_ready() : true;
    d.goep_state = g_goep_client ? int(g_goep_client->state) : -1;
    return d;
}

// BTstack is being torn down (radio powered off): everything registered with
// it is gone, so start() must set it all up again.
void detail::on_stack_deinit() {
    free(g_goep_client);   // BTstack's lists are being reset: the object is ours again
    g_goep_client = nullptr;
    g_stack_ready = false;
    g_sdp_handle = 0;
    g_powered = false;
    g_diag.hci_on = false;
    g_listening = false;
    g_rx_cid = 0;
    g_rx.closed();
    g_rx_reply = detail::ObexReceiver::Reply::None;
    if (g_tx != Tx::Idle) finish_send(SendState::Failed, "Bluetooth turned off");
    g_scanning = false;
    g_name_busy = false;
    g_name_queue.clear();
    g_pair_pending = false;
}

}  // namespace pd::bt
