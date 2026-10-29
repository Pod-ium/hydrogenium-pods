// PicoPods driver - ST7365P / ILI9488-class SPI panel (the PicoCalc's 320x320 IPS).
//
// The panel only accepts 18-bit colour over SPI (3 bytes per pixel; the 16-bit
// mode is silently ignored), the window setup and the pixels must share one
// chip-select transaction, and CS/DC edges need a few hundred ns to settle.
// From picocalc_ui's LCD driver (PicoCalc Writer + ClockworkPi reference).
#pragma once
#include "hardware/spi.h"
#include "pd/hal/display.h"

namespace pd::drivers {

class St7365p : public hal::Display {
public:
    struct Config {
        spi_inst_t* spi = spi1;
        uint sck = 10, mosi = 11, miso = 12, cs = 13, dc = 14, rst = 15;
        uint32_t hz = 25000000;       // ClockworkPi's speed
        uint8_t madctl = 0x48;        // column mirror + BGR
        bool invert = true;           // the PicoCalc panel needs INVON
        int width = 320, height = 320;
        uint32_t edge_cycles = 80;    // CS/DC settle time (~400 ns at 200 MHz)
    };

    void init(const Config& cfg);

    int width() const override { return cfg_.width; }
    int height() const override { return cfg_.height; }
    hal::PixelFormat format() const override { return hal::PixelFormat::RGB666; }
    void begin(int x0, int y0, int x1, int y1) override;
    void write(const uint8_t* data, size_t bytes) override;
    void write_async(const uint8_t* data, size_t bytes) override;
    bool busy() override;
    void end() override;
    void fill(int x, int y, int w, int h, Color c) override;

    int dma_channel() const { return dma_; }

private:
    void settle() const;
    void cs_low();
    void cs_high();
    void dc_cmd();
    void dc_data();
    void send(const uint8_t* d, size_t n);
    void cmd(uint8_t c, const uint8_t* data = nullptr, size_t n = 0);

    Config cfg_;
    bool in_data_ = false;
    int dma_ = -1;
};

}  // namespace pd::drivers
