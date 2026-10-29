// PicoPods terminal - ClockworkPi PicoCalc.
#include "pd/terminals/picocalc.h"

#include <cstdio>

#include "pd/fs.h"
#include "pd/sys.h"
#include "pico/stdlib.h"

namespace pd {

namespace {
drivers::St7365p g_lcd;
drivers::PicoCalcSouthBridge g_kbd;
drivers::SdSpi g_sd;
drivers::PwmAudio g_audio;
Terminal g_term;
bool g_ready = false;
}  // namespace

namespace picocalc {
drivers::St7365p& lcd() { return g_lcd; }
drivers::PicoCalcSouthBridge& southbridge() { return g_kbd; }
drivers::SdSpi& sd() { return g_sd; }
drivers::PwmAudio& pwm_audio() { return g_audio; }
}  // namespace picocalc

Terminal& terminal() { return g_term; }

Terminal& terminal_init(const TerminalConfig& cfg) {
    if (g_ready) return g_term;
    // Clock first: every peripheral below derives its timing from it.
    sys::set_clock_khz(cfg.clock_khz);
    if (cfg.debug_uart) stdio_init_all();

    drivers::St7365p::Config lc;
    lc.spi = PD_PICOCALC_LCD_SPI;
    lc.sck = PD_PICOCALC_LCD_SCK;
    lc.mosi = PD_PICOCALC_LCD_MOSI;
    lc.miso = PD_PICOCALC_LCD_MISO;
    lc.cs = PD_PICOCALC_LCD_CS;
    lc.dc = PD_PICOCALC_LCD_DC;
    lc.rst = PD_PICOCALC_LCD_RST;
    lc.hz = PD_PICOCALC_LCD_HZ;
    lc.invert = PD_PICOCALC_LCD_INVERT != 0;
    lc.madctl = PD_PICOCALC_LCD_MADCTL;
    g_lcd.init(lc);

    drivers::PicoCalcSouthBridge::Config kc;
    kc.i2c = PD_PICOCALC_KBD_I2C;
    kc.sda = PD_PICOCALC_KBD_SDA;
    kc.scl = PD_PICOCALC_KBD_SCL;
    kc.hz = PD_PICOCALC_KBD_HZ;
    kc.read_gap_us = PD_PICOCALC_KBD_GAP_US;
    if (!g_kbd.init(kc)) printf("[picopods] keyboard controller did not answer\n");

    g_term.name_ = "PicoCalc";
    g_term.display_ = &g_lcd;
    g_term.keyboard_device_ = &g_kbd;
    g_term.power_ = &g_kbd;
    g_term.keyboard_.attach(&g_kbd);

    if (cfg.storage) {
        drivers::SdSpi::Config sc;
        sc.spi = PD_PICOCALC_SD_SPI;
        sc.sck = PD_PICOCALC_SD_SCK;
        sc.mosi = PD_PICOCALC_SD_MOSI;
        sc.miso = PD_PICOCALC_SD_MISO;
        sc.cs = PD_PICOCALC_SD_CS;
        sc.detect = PD_PICOCALC_SD_DETECT;
        sc.hz = PD_PICOCALC_SD_HZ;
        g_sd.configure(sc);
        g_term.storage_ = &g_sd;
        fs::set_block_device(&g_sd);
    }
    if (cfg.audio) {
        drivers::PwmAudio::Config ac;
        ac.pin_l = PD_PICOCALC_AUDIO_L;
        ac.pin_r = PD_PICOCALC_AUDIO_R;
        g_audio.configure(ac);
        g_term.audio_ = &g_audio;
    }
    if (cfg.keyboard_on_core1) g_term.keyboard_.start_background();
    g_ready = true;
    return g_term;
}

}  // namespace pd
