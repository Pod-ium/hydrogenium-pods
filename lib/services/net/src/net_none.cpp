// pd::net on boards without a radio (Pico, Pico 2): every call fails cleanly
// with "No Wi-Fi on this board", so apps that offer Wi-Fi still build and run.
#include "pd/net.h"

namespace pd::net {
namespace {
bool none(std::string* err) {
    if (err) *err = "No Wi-Fi on this board";
    return false;
}
}  // namespace

bool start(std::string* err) { return none(err); }
void stop() {}
bool radio_acquire(std::string* err) { return none(err); }
void radio_release() {}
bool radio_on() { return false; }
bool is_on() { return false; }
void poll() {}
bool scan(std::vector<Network>& out, const KeepGoing&, std::string* err) {
    out.clear();
    return none(err);
}
bool connect(const std::string&, const std::string&, uint32_t, const KeepGoing&, std::string* err) { return none(err); }
void disconnect() {}
bool connected() { return false; }
std::string ip_address() { return ""; }
int rssi() { return 0; }
bool resolve(const std::string&, uint32_t*, uint32_t, const KeepGoing&, std::string* err) { return none(err); }
std::string ip_to_string(uint32_t ip) {
    return std::to_string(ip & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) +
           "." + std::to_string(ip >> 24);
}

Tcp::Tcp() : impl_(nullptr) {}
Tcp::~Tcp() {}
bool Tcp::connect(uint32_t, uint16_t, uint32_t, const KeepGoing&, std::string* err) { return none(err); }
bool Tcp::send(const void*, uint32_t, uint32_t, const KeepGoing&, std::string* err) { return none(err); }
int Tcp::recv(void*, uint32_t, uint32_t, const KeepGoing&, std::string* err) {
    none(err);
    return -1;
}
void Tcp::close() {}

}  // namespace pd::net
