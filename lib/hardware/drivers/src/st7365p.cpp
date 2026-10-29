// ST7365P driver. Timing notes from PicoCalc Writer: the controller needs a
// minimum CS-high time between transactions and setup time after CS/DC
// changes. Back-to-back GPIO toggles are too fast for it: it then merges
// transactions and misreads command bytes. Every edge is padded.
#include "pd/drivers/st7365p.h"

#include <initializer_list>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

namespace pd::drivers {

void St7365p::settle() const { busy_wait_at_least_cycles(cfg_.edge_cycles); }
void St7365p::cs_low() { gpio_put(cfg_.cs, 0); settle(); }
void St7365p::cs_high() { settle(); gpio_put(cfg_.cs, 1); settle(); }
void St7365p::dc_cmd() { gpio_put(cfg_.dc, 0); settle(); }
void St7365p::dc_data() { gpio_put(cfg_.dc, 1); settle(); }
void St7365p::send(const uint8_t* d, size_t n) { spi_write_blocking(cfg_.spi, d, n); }

void St7365p::cmd(uint8_t c, const uint8_t* data, size_t n) {
    if (in_data_) end();
    cs_low();
    dc_cmd();
    send(&c, 1);
    if (n) {
        dc_data();
        send(data, n);
    }
    cs_high();
}

void St7365p::init(const Config& cfg) {
    cfg_ = cfg;
    spi_init(cfg_.spi, cfg_.hz);
    spi_set_format(cfg_.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(cfg_.sck, GPIO_FUNC_SPI);
    gpio_set_function(cfg_.mosi, GPIO_FUNC_SPI);
    gpio_set_function(cfg_.miso, GPIO_FUNC_SPI);
    gpio_set_input_hysteresis_enabled(cfg_.miso, true);
    for (uint pin : {cfg_.cs, cfg_.dc, cfg_.rst}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 1);
    }
    sleep_ms(10);
    gpio_put(cfg_.rst, 0);
    sleep_ms(10);
    gpio_put(cfg_.rst, 1);
    sleep_ms(200);

    auto cmdl = [this](uint8_t c, std::initializer_list<uint8_t> d) { cmd(c, d.begin(), d.size()); };
    cmdl(0xE0, {0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F});
    cmdl(0xE1, {0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F});
    cmdl(0xC0, {0x17, 0x15});
    cmdl(0xC1, {0x41});
    cmdl(0xC5, {0x00, 0x12, 0x80});
    cmdl(0x36, {cfg_.madctl});
    cmdl(0x3A, {0x66});        // 18-bit colour; 16-bit (0x55) is silently ignored
    cmdl(0xB0, {0x00});
    cmdl(0xB1, {0xA0});
    cmd(cfg_.invert ? 0x21 : 0x20);
    cmdl(0xB4, {0x02});
    cmdl(0xB6, {0x02, 0x02, 0x3B});
    cmdl(0xB7, {0xC6});
    cmdl(0xE9, {0x00});
    cmdl(0xF7, {0xA9, 0x51, 0x2C, 0x82});
    cmd(0x11);
    sleep_ms(120);
    fill(0, 0, cfg_.width, cfg_.height, 0x000000);
    cmd(0x29);
    sleep_ms(120);
    cmdl(0x36, {cfg_.madctl});   // the reference driver re-applies this too

    if (dma_ < 0) dma_ = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(cfg_.spi, true));
    dma_channel_configure(dma_, &c, &spi_get_hw(cfg_.spi)->dr, nullptr, 0, false);
}

void St7365p::begin(int x0, int y0, int x1, int y1) {
    if (in_data_) end();
    const uint8_t ca[] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
    const uint8_t ra[] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
    static const uint8_t kCaset = 0x2A, kRaset = 0x2B, kRamwr = 0x2C;
    cs_low();
    dc_cmd(); send(&kCaset, 1);
    dc_data(); send(ca, 4);
    dc_cmd(); send(&kRaset, 1);
    dc_data(); send(ra, 4);
    dc_cmd(); send(&kRamwr, 1);
    dc_data();
    in_data_ = true;
}

void St7365p::write(const uint8_t* data, size_t bytes) { send(data, bytes); }

void St7365p::write_async(const uint8_t* data, size_t bytes) {
    dma_channel_set_read_addr(dma_, data, false);
    dma_channel_set_trans_count(dma_, bytes, true);
}

bool St7365p::busy() { return dma_ >= 0 && dma_channel_is_busy(dma_); }

void St7365p::end() {
    if (!in_data_) return;
    if (dma_ >= 0) dma_channel_wait_for_finish_blocking(dma_);
    in_data_ = false;
    // After DMA the SPI may still be shifting and the RX FIFO has overflowed.
    while (spi_is_busy(cfg_.spi)) tight_loop_contents();
    while (spi_is_readable(cfg_.spi)) (void)spi_get_hw(cfg_.spi)->dr;
    spi_get_hw(cfg_.spi)->icr = SPI_SSPICR_RORIC_BITS;
    cs_high();
}

void St7365p::fill(int x, int y, int w, int h, Color color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cfg_.width) w = cfg_.width - x;
    if (y + h > cfg_.height) h = cfg_.height - y;
    if (w <= 0 || h <= 0) return;
    static uint8_t row[480 * 3];
    if (w > 480) w = 480;
    uint8_t px[3] = {};
    encode(color, px);
    for (int i = 0; i < w; ++i) {
        row[3 * i] = px[0];
        row[3 * i + 1] = px[1];
        row[3 * i + 2] = px[2];
    }
    begin(x, y, x + w - 1, y + h - 1);
    for (int r = 0; r < h; ++r) write(row, size_t(w) * 3);
    end();
}

}  // namespace pd::drivers
