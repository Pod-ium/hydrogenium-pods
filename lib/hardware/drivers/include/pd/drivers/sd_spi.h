// PicoPods driver - SD / SDHC / SDXC card in SPI mode.
// From PicoCalc Writer (long busy timeouts, retries, CSD size) with PicoBoy's
// card-detect support.
#pragma once
#include "hardware/spi.h"
#include "pd/hal/block_device.h"

namespace pd::drivers {

class SdSpi : public hal::BlockDevice {
public:
    struct Config {
        spi_inst_t* spi = spi0;
        uint sck = 18, mosi = 19, miso = 16, cs = 17;
        int detect = 22;               // card-detect pin, -1 if none
        bool detect_active_low = true; // PicoCalc: the switch pulls low with a card in
        uint32_t hz = 25000000;
    };

    void configure(const Config& cfg);   // sets up the pins; init() talks to the card

    bool init() override;
    bool ready() const override { return ready_; }
    bool present() override;
    bool read(uint32_t sector, uint8_t* buf, uint32_t count) override;
    bool write(uint32_t sector, const uint8_t* buf, uint32_t count) override;
    bool sync() override;
    uint32_t sector_count() const override { return sectors_; }
    bool high_capacity() const { return high_cap_; }

private:
    uint8_t xfer(uint8_t b);
    void select();
    void deselect();
    bool wait_ready(uint32_t timeout_ms);
    uint8_t command(uint8_t cmd, uint32_t arg);
    bool read_data(uint8_t* buf, uint32_t n);
    bool write_data(const uint8_t* buf, uint8_t token);
    bool write_multi(uint32_t sector, const uint8_t* buf, uint32_t count);

    Config cfg_;
    bool pins_ = false;
    bool ready_ = false;
    bool high_cap_ = false;
    uint32_t sectors_ = 0;
};

}  // namespace pd::drivers
