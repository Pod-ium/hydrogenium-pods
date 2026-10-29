// SD card over SPI (PicoCalc Writer's driver, made pin-configurable).
#include "pd/drivers/sd_spi.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

namespace pd::drivers {

namespace {
// Cards may stay busy for a long time after writes (FAT updates, erasing
// clusters when a big file is deleted or truncated). 500 ms was too tight.
constexpr uint32_t kBusyMs = 2000;
constexpr uint32_t kReadTokenMs = 500;
constexpr int kRetries = 3;
}  // namespace

uint8_t SdSpi::xfer(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(cfg_.spi, &b, &r, 1);
    return r;
}
void SdSpi::select() { gpio_put(cfg_.cs, 0); xfer(0xFF); }
void SdSpi::deselect() { gpio_put(cfg_.cs, 1); xfer(0xFF); }

bool SdSpi::wait_ready(uint32_t timeout_ms) {
    const absolute_time_t until = make_timeout_time_ms(timeout_ms);
    do {
        if (xfer(0xFF) == 0xFF) return true;
    } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
    return false;
}

// Sends a command; leaves the card selected. Returns R1.
uint8_t SdSpi::command(uint8_t cmd, uint32_t arg) {
    if (cmd & 0x80) {   // ACMD: prefix with CMD55
        cmd &= 0x7F;
        const uint8_t r = command(55, 0);
        if (r > 1) return r;
    }
    if (cmd != 12) {
        deselect();
        select();
        if (cmd != 0 && !wait_ready(kBusyMs)) return 0xFF;
    }
    uint8_t pkt[6] = {uint8_t(0x40 | cmd), uint8_t(arg >> 24), uint8_t(arg >> 16), uint8_t(arg >> 8), uint8_t(arg),
                      0x01};
    if (cmd == 0) pkt[5] = 0x95;
    if (cmd == 8) pkt[5] = 0x87;
    spi_write_blocking(cfg_.spi, pkt, 6);
    if (cmd == 12) xfer(0xFF);   // skip stuff byte
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; ++i) {
        r = xfer(0xFF);
        if (!(r & 0x80)) break;
    }
    return r;
}

bool SdSpi::read_data(uint8_t* buf, uint32_t n) {
    const absolute_time_t until = make_timeout_time_ms(kReadTokenMs);
    uint8_t token;
    do {
        token = xfer(0xFF);
    } while (token == 0xFF && absolute_time_diff_us(get_absolute_time(), until) > 0);
    if (token != 0xFE) return false;
    spi_read_blocking(cfg_.spi, 0xFF, buf, n);
    xfer(0xFF);   // CRC
    xfer(0xFF);
    return true;
}

bool SdSpi::write_data(const uint8_t* buf, uint8_t token) {
    if (!wait_ready(kBusyMs)) return false;
    xfer(token);
    if (token == 0xFD) return true;   // stop-tran token has no payload
    spi_write_blocking(cfg_.spi, buf, 512);
    xfer(0xFF);   // dummy CRC
    xfer(0xFF);
    return (xfer(0xFF) & 0x1F) == 0x05;
}

void SdSpi::configure(const Config& cfg) {
    cfg_ = cfg;
    spi_init(cfg_.spi, 400 * 1000);
    spi_set_format(cfg_.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(cfg_.sck, GPIO_FUNC_SPI);
    gpio_set_function(cfg_.mosi, GPIO_FUNC_SPI);
    gpio_set_function(cfg_.miso, GPIO_FUNC_SPI);
    gpio_pull_up(cfg_.miso);
    gpio_init(cfg_.cs);
    gpio_set_dir(cfg_.cs, GPIO_OUT);
    gpio_put(cfg_.cs, 1);
    if (cfg_.detect >= 0) {
        gpio_init(uint(cfg_.detect));
        gpio_set_dir(uint(cfg_.detect), GPIO_IN);
        gpio_pull_up(uint(cfg_.detect));
    }
    pins_ = true;
}

bool SdSpi::present() {
    if (!pins_) configure(cfg_);
    if (cfg_.detect < 0) return true;
    const bool level = gpio_get(uint(cfg_.detect));
    return cfg_.detect_active_low ? !level : level;
}

bool SdSpi::init() {
    if (!pins_) configure(cfg_);
    ready_ = false;
    spi_set_baudrate(cfg_.spi, 400 * 1000);
    sleep_ms(2);
    gpio_put(cfg_.cs, 1);
    for (int i = 0; i < 10; ++i) xfer(0xFF);   // >= 74 clocks with CS high

    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 1; ++i) r = command(0, 0);
    if (r != 1) { deselect(); return false; }

    bool v2 = false;
    if (command(8, 0x1AA) == 1) {
        uint8_t ocr[4];
        spi_read_blocking(cfg_.spi, 0xFF, ocr, 4);
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) { deselect(); return false; }
        v2 = true;
    }
    const absolute_time_t until = make_timeout_time_ms(1500);
    do {
        r = command(0x80 | 41, v2 ? 0x40000000 : 0);
    } while (r == 1 && absolute_time_diff_us(get_absolute_time(), until) > 0);
    if (r != 0) { deselect(); return false; }

    high_cap_ = false;
    if (v2 && command(58, 0) == 0) {
        uint8_t ocr[4];
        spi_read_blocking(cfg_.spi, 0xFF, ocr, 4);
        high_cap_ = (ocr[0] & 0x40) != 0;
    }
    if (!high_cap_) command(16, 512);

    uint8_t csd[16];
    sectors_ = 0;
    if (command(9, 0) == 0 && read_data(csd, 16)) {
        if ((csd[0] >> 6) == 1) {
            const uint32_t cs = (uint32_t(csd[7] & 0x3F) << 16) | (uint32_t(csd[8]) << 8) | csd[9];
            sectors_ = (cs + 1) * 1024;
        } else {
            const uint32_t rbl = csd[5] & 15;
            const uint32_t cs = (uint32_t(csd[6] & 3) << 10) | (uint32_t(csd[7]) << 2) | (csd[8] >> 6);
            const uint32_t mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
            sectors_ = (cs + 1) << (mult + 2 + rbl - 9);
        }
    }
    deselect();
    spi_set_baudrate(cfg_.spi, cfg_.hz);
    ready_ = true;
    return true;
}

bool SdSpi::read(uint32_t sector, uint8_t* buff, uint32_t count) {
    if (!ready_) return false;
    for (int attempt = 0; attempt < kRetries; ++attempt) {
        const uint32_t addr = high_cap_ ? sector : sector * 512;
        bool ok;
        if (count == 1) {
            ok = command(17, addr) == 0 && read_data(buff, 512);
        } else {
            ok = command(18, addr) == 0;
            for (uint32_t i = 0; ok && i < count; ++i) ok = read_data(buff + i * 512, 512);
            command(12, 0);
            wait_ready(kBusyMs);   // CMD12 has a busy (R1b) response
        }
        deselect();
        if (ok) return true;
    }
    return false;
}

bool SdSpi::write_multi(uint32_t sector, const uint8_t* buff, uint32_t count) {
    // CMD25 + one 0xFC token per block + stop token: the card programs whole
    // blocks in one go instead of waiting after every sector.
    bool ok = command(25, high_cap_ ? sector : sector * 512) == 0;
    for (uint32_t i = 0; ok && i < count; ++i) ok = write_data(buff + i * 512, 0xFC);
    // Always send the stop token so the card leaves receive-data state.
    // The card may take one byte before it signals busy after the token.
    bool stopped = wait_ready(kBusyMs);
    xfer(0xFD);
    xfer(0xFF);
    stopped = wait_ready(kBusyMs) && stopped;
    deselect();
    return ok && stopped;
}

bool SdSpi::write(uint32_t sector, const uint8_t* buff, uint32_t count) {
    if (!ready_) return false;
    if (count > 1 && write_multi(sector, buff, count)) return true;
    // Single-block writes: also the fallback if a multi-block write failed
    // (the same data is simply written again).
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = sector + i;
        bool ok = false;
        for (int attempt = 0; !ok && attempt < kRetries; ++attempt) {
            ok = command(24, high_cap_ ? s : s * 512) == 0 && write_data(buff + i * 512, 0xFE) && wait_ready(kBusyMs);
            deselect();
        }
        if (!ok) return false;
    }
    return true;
}

bool SdSpi::sync() {
    if (!ready_) return false;
    select();
    const bool ok = wait_ready(kBusyMs);
    deselect();
    return ok;
}

}  // namespace pd::drivers
