// pd::net on the Pico W / Pico 2 W: CYW43 radio + lwIP in poll mode.
#include <algorithm>
#include <cstring>

#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pd/net.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

namespace pd::net {
namespace {

bool g_on = false;

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

// Pumps the stack until done() or timeout / cancel. Checks keepGoing about
// every 100 ms (reading the keyboard is slow).
template <typename F>
bool waitFor(F done, uint32_t timeoutMs, const KeepGoing& keepGoing, bool* cancelled) {
    const uint32_t start = nowMs();
    uint32_t nextCheck = start + 100;
    *cancelled = false;
    for (;;) {
        cyw43_arch_poll();
        if (done()) return true;
        const uint32_t t = nowMs();
        if (t - start >= timeoutMs) return false;
        if (int32_t(t - nextCheck) >= 0) {
            nextCheck = t + 100;
            if (keepGoing && !keepGoing()) {
                *cancelled = true;
                return false;
            }
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(5));
    }
}

struct ScanCtx {
    std::vector<Network>* out;
};

int scanResult(void* env, const cyw43_ev_scan_result_t* r) {
    auto* ctx = static_cast<ScanCtx*>(env);
    if (!r || r->ssid_len == 0) return 0;
    std::string ssid(reinterpret_cast<const char*>(r->ssid), r->ssid_len);
    for (Network& n : *ctx->out) {
        if (n.ssid == ssid) {
            if (r->rssi > n.rssi) n.rssi = r->rssi;
            return 0;
        }
    }
    if (ctx->out->size() < 40) ctx->out->push_back(Network{ssid, r->rssi, r->auth_mode != 0});
    return 0;
}

}  // namespace

// The CYW43 chip carries Wi-Fi and Bluetooth: each user holds it, and it
// powers down only when nobody does.
int g_holds = 0;

bool radio_acquire(std::string* err) {
    if (g_holds == 0 && cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE) != 0) {
        if (err) *err = "radio chip did not start";
        return false;
    }
    ++g_holds;
    return true;
}

void radio_release() {
    if (g_holds == 0) return;
    if (--g_holds == 0) cyw43_arch_deinit();
}

bool radio_on() { return g_holds > 0; }

bool start(std::string* err) {
    if (g_on) return true;
    if (!radio_acquire(err)) return false;
    cyw43_arch_enable_sta_mode();
    g_on = true;
    return true;
}

void stop() {
    if (!g_on) return;
    cyw43_arch_disable_sta_mode();
    g_on = false;
    radio_release();
}

bool is_on() { return g_on; }

void poll() {
    if (g_holds) cyw43_arch_poll();
}

bool scan(std::vector<Network>& out, const KeepGoing& keepGoing, std::string* err) {
    out.clear();
    if (!start(err)) return false;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof opts);
    ScanCtx ctx{&out};
    if (cyw43_wifi_scan(&cyw43_state, &opts, &ctx, scanResult) != 0) {
        if (err) *err = "Scan failed to start";
        return false;
    }
    bool cancelled = false;
    const bool ok = waitFor([] { return !cyw43_wifi_scan_active(&cyw43_state); }, 15000, keepGoing,
                            &cancelled);
    if (!ok) {
        if (err) *err = cancelled ? "Cancelled" : "Scan timed out";
        return false;
    }
    // strongest first
    for (size_t i = 1; i < out.size(); ++i)
        for (size_t j = i; j > 0 && out[j].rssi > out[j - 1].rssi; --j) std::swap(out[j], out[j - 1]);
    return true;
}

bool connect(const std::string& ssid, const std::string& pass, uint32_t timeoutMs,
             const KeepGoing& keepGoing, std::string* err) {
    if (!start(err)) return false;
    if (connected()) return true;
    // WPA/WPA2 first (most routers); if that is refused, try WPA2/WPA3 for
    // WPA3-only networks.
    const uint32_t auths[2] = {pass.empty() ? uint32_t(CYW43_AUTH_OPEN) : uint32_t(CYW43_AUTH_WPA2_MIXED_PSK),
                               uint32_t(CYW43_AUTH_WPA3_WPA2_AES_PSK)};
    const int tries = pass.empty() ? 1 : 2;
    int status = CYW43_LINK_DOWN;
    bool cancelled = false;
    for (int i = 0; i < tries && !cancelled; ++i) {
        if (i) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        if (cyw43_arch_wifi_connect_async(ssid.c_str(), pass.empty() ? nullptr : pass.c_str(), auths[i]) != 0) {
            if (err) *err = "Could not start connecting";
            return false;
        }
        const bool ok = waitFor(
            [&] {
                status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                return status == CYW43_LINK_UP || status == CYW43_LINK_FAIL ||
                       status == CYW43_LINK_NONET || status == CYW43_LINK_BADAUTH;
            },
            timeoutMs, keepGoing, &cancelled);
        if (ok && status == CYW43_LINK_UP) {
            cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
            return true;
        }
        if (!ok || status == CYW43_LINK_NONET) break;  // timeout / cancel / no such network
    }
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    if (err) {
        if (cancelled) *err = "Cancelled";
        else if (status == CYW43_LINK_BADAUTH) *err = "Wrong password";
        else if (status == CYW43_LINK_NONET) *err = "Network not found";
        else if (status == CYW43_LINK_FAIL) *err = "Connection failed";
        else if (status == CYW43_LINK_NOIP) *err = "No IP address (DHCP)";
        else *err = "Timed out";
    }
    return false;
}

void disconnect() {
    if (g_on) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}

bool connected() {
    return g_on && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

std::string ip_address() {
    if (!connected()) return "";
    return ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
}

int rssi() {
    int32_t r = 0;
    if (!connected() || cyw43_wifi_get_rssi(&cyw43_state, &r) != 0) return 0;
    return int(r);
}

std::string ip_to_string(uint32_t ip) {
    ip4_addr_t a;
    a.addr = ip;
    return ip4addr_ntoa(&a);
}

namespace {
struct DnsCtx {
    bool done = false, ok = false;
    uint32_t ip = 0;
};
void dnsFound(const char*, const ip_addr_t* addr, void* arg) {
    auto* c = static_cast<DnsCtx*>(arg);
    c->done = true;
    if (addr) {
        c->ok = true;
        c->ip = ip4_addr_get_u32(ip_2_ip4(addr));
    }
}
}  // namespace

bool resolve(const std::string& host, uint32_t* ip, uint32_t timeoutMs, const KeepGoing& keepGoing,
             std::string* err) {
    ip4_addr_t a;
    if (ip4addr_aton(host.c_str(), &a)) {
        *ip = a.addr;
        return true;
    }
    if (!connected()) {
        if (err) *err = "Wi-Fi not connected";
        return false;
    }
    DnsCtx ctx;
    ip_addr_t addr;
    const err_t e = dns_gethostbyname(host.c_str(), &addr, dnsFound, &ctx);
    if (e == ERR_OK) {
        *ip = ip4_addr_get_u32(ip_2_ip4(&addr));
        return true;
    }
    if (e != ERR_INPROGRESS) {
        if (err) *err = "Cannot resolve " + host;
        return false;
    }
    bool cancelled = false;
    waitFor([&] { return ctx.done; }, timeoutMs, keepGoing, &cancelled);
    if (ctx.ok) {
        *ip = ctx.ip;
        return true;
    }
    if (err) *err = cancelled ? "Cancelled" : "Cannot find " + host;
    return false;
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------

namespace {
struct Conn {
    tcp_pcb* pcb = nullptr;
    bool connected = false, closed = false, failed = false;
    pbuf* rx = nullptr;
    uint16_t rxOff = 0;
};

err_t onConnected(void* arg, tcp_pcb*, err_t err) {
    auto* c = static_cast<Conn*>(arg);
    if (err == ERR_OK) c->connected = true;
    else c->failed = true;
    return ERR_OK;
}
err_t onRecv(void* arg, tcp_pcb* pcb, pbuf* p, err_t err) {
    auto* c = static_cast<Conn*>(arg);
    if (!p) {
        c->closed = true;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    if (c->rx) pbuf_cat(c->rx, p);
    else c->rx = p;
    (void)pcb;
    return ERR_OK;
}
void onErr(void* arg, err_t) {
    auto* c = static_cast<Conn*>(arg);
    c->pcb = nullptr;  // already freed by lwIP
    c->failed = true;
}
}  // namespace

Tcp::Tcp() : impl_(new Conn) {}
Tcp::~Tcp() {
    close();
    delete static_cast<Conn*>(impl_);
}

bool Tcp::connect(uint32_t ip, uint16_t port, uint32_t timeoutMs, const KeepGoing& keepGoing,
                  std::string* err) {
    close();
    Conn* c = static_cast<Conn*>(impl_);
    *c = Conn();
    c->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!c->pcb) {
        if (err) *err = "Out of network memory";
        return false;
    }
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, onRecv);
    tcp_err(c->pcb, onErr);
    ip_addr_t addr;
    ip_addr_set_ip4_u32(&addr, ip);
    if (tcp_connect(c->pcb, &addr, port, onConnected) != ERR_OK) {
        if (err) *err = "Cannot connect";
        close();
        return false;
    }
    bool cancelled = false;
    waitFor([&] { return c->connected || c->failed; }, timeoutMs, keepGoing, &cancelled);
    if (c->connected) return true;
    if (err) *err = cancelled ? "Cancelled" : c->failed ? "Connection refused" : "Connection timed out";
    close();
    return false;
}

bool Tcp::send(const void* data, uint32_t n, uint32_t timeoutMs, const KeepGoing& keepGoing,
               std::string* err) {
    Conn* c = static_cast<Conn*>(impl_);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        if (!c->pcb || c->failed) {
            if (err) *err = "Connection lost";
            return false;
        }
        uint32_t room = 0;
        bool cancelled = false;
        const bool ok = waitFor(
            [&] {
                if (!c->pcb || c->failed) return true;
                room = tcp_sndbuf(c->pcb);
                return room > 0 && tcp_sndqueuelen(c->pcb) < TCP_SND_QUEUELEN - 2;
            },
            timeoutMs, keepGoing, &cancelled);
        if (!ok || !c->pcb || c->failed) {
            if (err) *err = cancelled ? "Cancelled" : (!c->pcb || c->failed) ? "Connection lost" : "Send timed out";
            return false;
        }
        const uint32_t k = std::min<uint32_t>(n, std::min<uint32_t>(room, TCP_MSS));
        const err_t e = tcp_write(c->pcb, p, uint16_t(k), TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM) {
            tcp_output(c->pcb);
            cyw43_arch_poll();
            continue;
        }
        if (e != ERR_OK) {
            if (err) *err = "Send error";
            return false;
        }
        p += k;
        n -= k;
        if (n == 0 || tcp_sndbuf(c->pcb) < TCP_MSS) tcp_output(c->pcb);
    }
    if (c->pcb) tcp_output(c->pcb);
    return true;
}

int Tcp::recv(void* buf, uint32_t n, uint32_t timeoutMs, const KeepGoing& keepGoing, std::string* err) {
    Conn* c = static_cast<Conn*>(impl_);
    bool cancelled = false;
    waitFor([&] { return c->rx || c->closed || c->failed; }, timeoutMs, keepGoing, &cancelled);
    if (c->rx) {
        const uint16_t avail = uint16_t(c->rx->tot_len - c->rxOff);
        const uint16_t k = uint16_t(std::min<uint32_t>(n, avail));
        pbuf_copy_partial(c->rx, buf, k, c->rxOff);
        c->rxOff = uint16_t(c->rxOff + k);
        if (c->pcb) tcp_recved(c->pcb, k);
        if (c->rxOff == c->rx->tot_len) {
            pbuf_free(c->rx);
            c->rx = nullptr;
            c->rxOff = 0;
        }
        return k;
    }
    if (c->closed) return 0;
    if (err) *err = cancelled ? "Cancelled" : c->failed ? "Connection lost" : "No reply (timed out)";
    return -1;
}

void Tcp::close() {
    Conn* c = static_cast<Conn*>(impl_);
    if (c->pcb) {
        tcp_arg(c->pcb, nullptr);
        tcp_recv(c->pcb, nullptr);
        tcp_err(c->pcb, nullptr);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        c->pcb = nullptr;
    }
    if (c->rx) {
        pbuf_free(c->rx);
        c->rx = nullptr;
    }
    c->rxOff = 0;
    c->connected = false;
}

}  // namespace pd::net
