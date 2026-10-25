// pd_bt on boards without a Bluetooth radio.
#include "pd/bt.h"

namespace pd::bt {
const char* Device::kind() const { return "device"; }
bool available() { return false; }
void set_key_file(const std::string&) {}
void set_device_name(const std::string&) {}
bool start(std::string* err) {
    if (err) *err = "No Bluetooth on this board";
    return false;
}
void stop() {}
bool powered() { return false; }
void poll() {}
std::string address() { return ""; }
bool pair_request(PairRequest*) { return false; }
void pair_answer(bool) {}
bool scan_start() { return false; }
void scan_stop() {}
bool scanning() { return false; }
std::string scan_error() { return ""; }
std::vector<Device> devices() { return {}; }
void forget(const std::string&) {}
bool receive_start(const std::string&) { return false; }
void receive_stop() {}
ReceiveStatus receive_status() { return ReceiveStatus(); }
bool send_start(const std::string&, const std::vector<std::string>&) { return false; }
void send_cancel() {}
SendStatus send_status() { return SendStatus(); }
Diag diag() { return Diag(); }
}  // namespace pd::bt
