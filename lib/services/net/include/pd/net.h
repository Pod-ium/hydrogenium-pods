// PicoPods - Wi-Fi and a minimal blocking TCP client (from PicoCalc Writer).
//
// On the device this drives the Pico 2 W's CYW43 radio through lwIP in
// "poll" mode (src/net_cyw43.cpp): nothing runs in the background, every call
// below pumps the network stack itself while it waits, and the rest of the
// time the app calls net::poll() from its main loop. On the host the same API is backed by
// POSIX sockets (src/net_posix.cpp) so the upload code can be tested
// against a mock CrossPoint server.
//
// Every waiting call takes a `keepGoing` callback, polled a few times per
// second, that returns false to cancel (the UI checks the Esc key).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pd::net {

using KeepGoing = std::function<bool()>;

struct Network {
    std::string ssid;
    int rssi;     // dBm
    bool secure;  // needs a password
};

// The radio chip (CYW43: Wi-Fi + Bluetooth). Each user holds it; it powers
// down when the last hold is released. start()/stop() hold it for Wi-Fi.
bool radio_acquire(std::string* err);
void radio_release();
bool radio_on();

bool start(std::string* err);  // powers the radio up (station mode)
void stop();                   // powers it down
bool is_on();
void poll();                   // pump the stack; cheap when off

bool scan(std::vector<Network>& out, const KeepGoing& keepGoing, std::string* err);
bool connect(const std::string& ssid, const std::string& pass, uint32_t timeoutMs,
             const KeepGoing& keepGoing, std::string* err);
void disconnect();  // leaves the network, radio stays on
bool connected();
std::string ip_address();  // "" when not connected
int rssi();               // dBm, 0 if unknown

// Resolves a host name (dotted IPv4 and *.local mDNS names included).
bool resolve(const std::string& host, uint32_t* ip, uint32_t timeoutMs, const KeepGoing& keepGoing,
             std::string* err);
std::string ip_to_string(uint32_t ip);  // ip is in network byte order

class Tcp {
public:
    Tcp();
    ~Tcp();
    Tcp(const Tcp&) = delete;
    Tcp& operator=(const Tcp&) = delete;

    bool connect(uint32_t ip, uint16_t port, uint32_t timeoutMs, const KeepGoing& keepGoing,
                 std::string* err);
    // Sends everything (blocking while the send buffer is full).
    bool send(const void* data, uint32_t n, uint32_t timeoutMs, const KeepGoing& keepGoing,
              std::string* err);
    // Returns bytes read, 0 when the peer closed the connection, -1 on error
    // or timeout.
    int recv(void* buf, uint32_t n, uint32_t timeoutMs, const KeepGoing& keepGoing,
             std::string* err);
    void close();

private:
    void* impl_;
};

}  // namespace pd::net
