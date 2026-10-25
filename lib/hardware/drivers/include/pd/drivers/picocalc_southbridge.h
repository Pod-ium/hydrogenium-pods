// PicoPods driver - the PicoCalc "south bridge": an STM32 on I2C that scans the
// keyboard and measures the battery.
//
// Protocol (ClockworkPi reference code): write a register number, wait, read
// two bytes. Register 0x09 pops the key FIFO as [state, code] (0,0 = empty);
// register 0x0B returns the battery in byte 1 (bit 7 = charging). The
// controller needs a slow bus (10 kHz) and a long gap between write and read
// (16 ms in the reference code and the UF2 Loader).
//
// Both interfaces may be used from different cores: I2C access is serialised
// by a mutex.
#pragma once
#include <atomic>

#include "hardware/i2c.h"
#include "pd/hal/keyboard.h"
#include "pd/hal/power.h"
#include "pico/mutex.h"

namespace pd::drivers {

class PicoCalcSouthBridge : public hal::KeyboardDevice, public hal::Power {
public:
    struct Config {
        i2c_inst_t* i2c = i2c1;
        uint sda = 6, scl = 7;
        uint32_t hz = 10000;
        uint8_t addr = 0x1F;
        uint32_t read_gap_us = 16000;        // lower = snappier, but some firmware versions misread
        uint32_t battery_period_ms = 30000;  // how often the cached battery value is refreshed
    };

    bool init(const Config& cfg);

    // KeyboardDevice
    bool read(hal::RawKey& ev) override;
    uint32_t poll_interval_ms() const override { return 4; }

    // Power (cached; refreshed every battery_period_ms)
    int battery_percent() override;
    bool charging() override;

    // Raw register read: two bytes. For registers this driver does not wrap.
    bool read_reg(uint8_t reg, uint8_t out[2]);

private:
    bool read_reg_locked(uint8_t reg, uint8_t out[2]);
    void refresh_battery(bool force);

    Config cfg_;
    mutex_t lock_;
    std::atomic<int> battery_{-1};   // raw register value, bit 7 = charging
    std::atomic<uint32_t> next_battery_{0};
};

}  // namespace pd::drivers
