// PicoCalc keyboard + battery (merged from PicoCalc Writer and PicoBoy).
#include "pd/drivers/picocalc_southbridge.h"

#include "hardware/gpio.h"
#include "pd/sys.h"
#include "pico/stdlib.h"

namespace pd::drivers {

namespace {
constexpr uint8_t kRegFifo = 0x09;
constexpr uint8_t kRegBattery = 0x0B;
constexpr uint32_t kTimeoutUs = 50000;
}  // namespace

bool PicoCalcSouthBridge::init(const Config& cfg) {
    cfg_ = cfg;
    mutex_init(&lock_);
    i2c_init(cfg_.i2c, cfg_.hz);
    gpio_set_function(cfg_.sda, GPIO_FUNC_I2C);
    gpio_set_function(cfg_.scl, GPIO_FUNC_I2C);
    gpio_pull_up(cfg_.sda);
    gpio_pull_up(cfg_.scl);
    // Drain anything queued during boot.
    uint8_t b[2];
    bool alive = false;
    for (int i = 0; i < 32 && read_reg(kRegFifo, b); ++i) {
        alive = true;
        if (b[0] == 0 && b[1] == 0) break;
    }
    refresh_battery(true);
    return alive;
}

bool PicoCalcSouthBridge::read_reg_locked(uint8_t reg, uint8_t out[2]) {
    if (i2c_write_timeout_us(cfg_.i2c, cfg_.addr, &reg, 1, false, kTimeoutUs) != 1) return false;
    sleep_us(cfg_.read_gap_us);
    out[0] = out[1] = 0;
    return i2c_read_timeout_us(cfg_.i2c, cfg_.addr, out, 2, false, kTimeoutUs) == 2;
}

bool PicoCalcSouthBridge::read_reg(uint8_t reg, uint8_t out[2]) {
    mutex_enter_blocking(&lock_);
    const bool ok = read_reg_locked(reg, out);
    mutex_exit(&lock_);
    return ok;
}

void PicoCalcSouthBridge::refresh_battery(bool force) {
    if (!force && !sys::reached(next_battery_.load())) return;
    next_battery_ = sys::now_ms() + cfg_.battery_period_ms;
    uint8_t b[2];
    if (read_reg(kRegBattery, b)) battery_ = b[1];
}

bool PicoCalcSouthBridge::read(hal::RawKey& ev) {
    refresh_battery(false);
    uint8_t b[2];
    if (!read_reg(kRegFifo, b)) return false;
    if (b[0] == 0 && b[1] == 0) return false;   // FIFO empty
    ev.state = b[0];
    ev.code = b[1];
    return true;
}

int PicoCalcSouthBridge::battery_percent() {
    refresh_battery(false);
    const int v = battery_.load();
    return v < 0 ? -1 : (v & 0x7F);
}

bool PicoCalcSouthBridge::charging() {
    const int v = battery_.load();
    return v >= 0 && (v & 0x80);
}

}  // namespace pd::drivers
