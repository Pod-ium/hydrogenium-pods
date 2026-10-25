// PicoPods - Wi-Fi with a user interface: saved credentials, network picker,
// connect-on-demand and automatic power-off (PicoCalc Writer's Wi-Fi flow).
//
//   pd::WifiManager wifi("/.myapp/wifi.txt");
//   wifi.load();
//   ...
//   if (wifi.ensure(ui)) { /* connected: use pd::net::Tcp ... */ }
//   ...
//   wifi.tick();   // in the main loop: pumps lwIP, turns the radio off when idle
//
// Credentials are stored in plain text ("ssid=", "pass=") on the SD card.
#pragma once
#include <string>

#include "pd/net.h"
#include "pd/ui.h"

namespace pd {

class WifiManager {
public:
    explicit WifiManager(std::string credentials_path) : path_(std::move(credentials_path)) {}

    uint32_t idle_off_ms = 10 * 60 * 1000;   // 0 = never
    uint32_t connect_timeout_ms = 20000;

    void load();
    void save();
    void forget();
    const std::string& ssid() const { return ssid_; }

    // Connected already, or connects with the saved network, or asks. True
    // when connected. Each attempt shows a panel; Esc cancels.
    bool ensure(ui::Context& ui);
    // Scans, lets the user pick a network and type its password, connects.
    bool choose(ui::Context& ui);
    // Status + Connect / Choose / Turn off / Forget, as a menu.
    void menu(ui::Context& ui);

    // Marks the radio as in use (postpones the idle power-off).
    void used();
    // Call often from the main loop. Returns true when it just turned the
    // radio off because it was idle.
    bool tick();

private:
    net::KeepGoing checker(ui::Context& ui, const std::string& what);

    std::string path_, ssid_, pass_;
    uint32_t off_at_ = 0;
};

}  // namespace pd
