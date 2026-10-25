// PicoPods - Wi-Fi manager (from PicoCalc Writer's app_share.cpp).
#include "pd/wifi.h"

#include <cstdio>

#include "pd/fs.h"
#include "pd/settings.h"
#include "pd/sys.h"

namespace pd {

void WifiManager::load() {
    Settings s;
    if (!s.load(path_.c_str())) return;
    ssid_ = s.get("ssid");
    pass_ = s.get("pass");
}

void WifiManager::save() {
    Settings s;
    s.set("ssid", ssid_);
    s.set("pass", pass_);
    s.save(path_.c_str());
}

void WifiManager::forget() {
    net::stop();
    ssid_.clear();
    pass_.clear();
    fs::remove(path_.c_str());
}

void WifiManager::used() { off_at_ = sys::now_ms() + idle_off_ms; }

bool WifiManager::tick() {
    if (!net::is_on()) return false;
    net::poll();
    if (idle_off_ms && off_at_ && sys::reached(off_at_)) {
        net::stop();
        off_at_ = 0;
        return true;
    }
    return false;
}

net::KeepGoing WifiManager::checker(ui::Context& ui, const std::string& what) {
    ui::busy(ui, "Wi-Fi", what);
    return ui.kbd.esc_checker();
}

bool WifiManager::choose(ui::Context& ui) {
    std::vector<net::Network> nets;
    std::string err;
    if (!net::scan(nets, checker(ui, "Scanning for networks..."), &err)) {
        ui::message(ui, "Wi-Fi", {"Scan failed: " + err});
        return false;
    }
    // Labels must outlive the menu: build all strings first, then point at them.
    std::vector<std::string> right(nets.size());
    for (size_t i = 0; i < nets.size(); ++i) {
        char r[16];
        snprintf(r, sizeof r, "%s%d", nets[i].secure ? "* " : "", nets[i].rssi);
        right[i] = r;
    }
    std::vector<ui::MenuItem> items;
    for (size_t i = 0; i < nets.size(); ++i) items.push_back(ui::action(nets[i].ssid.c_str(), right[i].c_str()));
    items.push_back(ui::action("Other network..."));
    const int r = ui::menu(ui, "Wi-Fi networks  (* = password)", items);
    if (r < 0) return false;
    std::string ssid, pass;
    bool secure = true;
    if (r < int(nets.size())) {
        ssid = nets[size_t(r)].ssid;
        secure = nets[size_t(r)].secure;
    } else {
        ssid = ssid_;
        if (!ui::prompt(ui, "Network name (SSID)", ssid) || ssid.empty()) return false;
    }
    if (secure) {
        pass = ssid == ssid_ ? pass_ : "";
        if (!ui::prompt(ui, ("Password for " + ssid).c_str(), pass, "Enter connect  Tab show  Esc", true)) return false;
    }
    net::disconnect();
    if (!net::connect(ssid, pass, connect_timeout_ms, checker(ui, "Connecting to " + ssid + "..."), &err)) {
        ui::message(ui, "Wi-Fi", {"Could not connect to " + ssid + ":", err});
        return false;
    }
    ssid_ = ssid;
    pass_ = pass;
    save();
    used();
    return true;
}

bool WifiManager::ensure(ui::Context& ui) {
    if (net::connected()) {
        used();
        return true;
    }
    if (!ssid_.empty()) {
        std::string err;
        if (net::connect(ssid_, pass_, connect_timeout_ms, checker(ui, "Connecting to " + ssid_ + "..."), &err)) {
            used();
            return true;
        }
        if (err == "Cancelled") return false;
        if (ui::ask(ui, "Wi-Fi", "Could not connect to " + ssid_ + " (" + err + "). Choose another network?") != 1)
            return false;
    }
    return choose(ui);
}

void WifiManager::menu(ui::Context& ui) {
    const bool on = net::connected();
    const std::string state =
        on ? "Connected: " + ssid_ : (ssid_.empty() ? "Not set up" : "Off (" + ssid_ + ")");
    std::string info;
    if (on) {
        char b[48];
        snprintf(b, sizeof b, "%s  %d dBm", net::ip_address().c_str(), net::rssi());
        info = b;
    }
    std::vector<ui::MenuItem> items = {
        ui::info(state.c_str()),
        on ? ui::info(info.c_str()) : ui::action("Connect", "Enter"),
        ui::action("Choose network..."),
        ui::action("Turn Wi-Fi off"),
        ui::action("Forget saved network"),
    };
    items[3].enabled = net::is_on();   // the radio may be on without a connection
    items[4].enabled = !ssid_.empty();
    switch (ui::menu(ui, "Wi-Fi", items)) {
        case 1: ensure(ui); break;
        case 2: choose(ui); break;
        case 3: net::stop(); break;
        case 4:
            if (ui::ask(ui, "Wi-Fi", "Forget " + ssid_ + " and its password?", false) == 1) forget();
            break;
        default: break;
    }
}

}  // namespace pd
