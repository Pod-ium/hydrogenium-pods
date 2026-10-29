// pd::net for the host: POSIX sockets, no radio. NET_FAKE_SSID / NET_FAKE_PASS
// (environment) set the network that "exists"; resolve() also maps
// crosspoint.local to 127.0.0.1 so the mock server can stand in for the X3.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "pd/net.h"

namespace pd::net {
namespace {
bool g_on = false, g_conn = false;
uint32_t nowMs() {
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
const char* env(const char* k, const char* d) {
    const char* v = getenv(k);
    return v ? v : d;
}
}  // namespace

bool start(std::string*) { g_on = true; return true; }
void stop() { g_on = false; g_conn = false; }
bool radio_acquire(std::string*) { return true; }
void radio_release() {}
bool radio_on() { return true; }
bool is_on() { return g_on; }
void poll() {}

bool scan(std::vector<Network>& out, const KeepGoing&, std::string*) {
    g_on = true;
    out = {{env("NET_FAKE_SSID", "HomeNet"), -48, true}, {"Neighbours", -80, true}, {"Cafe", -70, false}};
    return true;
}
bool connect(const std::string& ssid, const std::string& pass, uint32_t, const KeepGoing&, std::string* err) {
    g_on = true;
    if (ssid != env("NET_FAKE_SSID", "HomeNet")) { if (err) *err = "Network not found"; return false; }
    if (pass != env("NET_FAKE_PASS", "secret")) { if (err) *err = "Wrong password"; return false; }
    g_conn = true;
    return true;
}
void disconnect() { g_conn = false; }
bool connected() { return g_on && g_conn; }
std::string ip_address() { return connected() ? "192.168.1.42" : ""; }
int rssi() { return connected() ? -48 : 0; }

std::string ip_to_string(uint32_t ip) {
    in_addr a;
    a.s_addr = ip;
    return inet_ntoa(a);
}

bool resolve(const std::string& host, uint32_t* ip, uint32_t, const KeepGoing&, std::string* err) {
    in_addr a;
    if (inet_aton(host.c_str(), &a)) { *ip = a.s_addr; return true; }
    if (!connected()) { if (err) *err = "Wi-Fi not connected"; return false; }
    if (host == "crosspoint.local") { *ip = htonl(0x7F000001); return true; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (err) *err = "Cannot find " + host;
        return false;
    }
    *ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

namespace {
struct Sock { int fd = -1; };
bool waitFd(int fd, short ev, uint32_t timeoutMs, const KeepGoing& kg, std::string* err) {
    const uint32_t start = nowMs();
    for (;;) {
        pollfd p{fd, ev, 0};
        const int r = ::poll(&p, 1, 50);
        if (r > 0) return true;
        if (nowMs() - start >= timeoutMs) { if (err) *err = "Timed out"; return false; }
        if (kg && !kg()) { if (err) *err = "Cancelled"; return false; }
    }
}
}  // namespace

Tcp::Tcp() : impl_(new Sock) {}
Tcp::~Tcp() { close(); delete static_cast<Sock*>(impl_); }

bool Tcp::connect(uint32_t ip, uint16_t port, uint32_t timeoutMs, const KeepGoing& kg, std::string* err) {
    close();
    Sock* s = static_cast<Sock*>(impl_);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(s->fd, F_SETFL, O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    ::connect(s->fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (!waitFd(s->fd, POLLOUT, timeoutMs, kg, err)) { close(); return false; }
    int e = 0;
    socklen_t l = sizeof e;
    getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &l);
    if (e) { if (err) *err = "Connection refused"; close(); return false; }
    return true;
}

bool Tcp::send(const void* data, uint32_t n, uint32_t timeoutMs, const KeepGoing& kg, std::string* err) {
    Sock* s = static_cast<Sock*>(impl_);
    const char* p = static_cast<const char*>(data);
    while (n) {
        if (!waitFd(s->fd, POLLOUT, timeoutMs, kg, err)) return false;
        const ssize_t k = ::send(s->fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EAGAIN) continue;
            if (err) *err = "Connection lost";
            return false;
        }
        p += k;
        n -= uint32_t(k);
    }
    return true;
}

int Tcp::recv(void* buf, uint32_t n, uint32_t timeoutMs, const KeepGoing& kg, std::string* err) {
    Sock* s = static_cast<Sock*>(impl_);
    if (!waitFd(s->fd, POLLIN, timeoutMs, kg, err)) return -1;
    const ssize_t k = ::recv(s->fd, buf, n, 0);
    if (k < 0) { if (err) *err = "Connection lost"; return -1; }
    return int(k);
}

void Tcp::close() {
    Sock* s = static_cast<Sock*>(impl_);
    if (s->fd >= 0) ::close(s->fd);
    s->fd = -1;
}

}  // namespace pd::net
