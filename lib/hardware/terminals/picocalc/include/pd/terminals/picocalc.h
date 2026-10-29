// PicoPods terminal - ClockworkPi PicoCalc (mainboard v2.0) with a Pico-format
// board (Pico, Pico W, Pico 2, Pico 2 W, compatible RP2040/RP2350 boards).
//
// Pin map from ClockworkPi's reference code and the UF2 Loader sources. Every
// value can be overridden with a compile definition, e.g.
//   target_compile_definitions(my_app PRIVATE PD_PICOCALC_AUDIO_L=26)
// for older mainboards that route the left channel to GP26.
#pragma once
#include "pd/drivers/picocalc_southbridge.h"
#include "pd/drivers/pwm_audio.h"
#include "pd/drivers/sd_spi.h"
#include "pd/drivers/st7365p.h"
#include "pd/terminal.h"

// ---- display: 320x320 IPS, ST7365P on SPI1
#ifndef PD_PICOCALC_LCD_SPI
#define PD_PICOCALC_LCD_SPI spi1
#endif
#ifndef PD_PICOCALC_LCD_SCK
#define PD_PICOCALC_LCD_SCK 10
#endif
#ifndef PD_PICOCALC_LCD_MOSI
#define PD_PICOCALC_LCD_MOSI 11
#endif
#ifndef PD_PICOCALC_LCD_MISO
#define PD_PICOCALC_LCD_MISO 12
#endif
#ifndef PD_PICOCALC_LCD_CS
#define PD_PICOCALC_LCD_CS 13
#endif
#ifndef PD_PICOCALC_LCD_DC
#define PD_PICOCALC_LCD_DC 14
#endif
#ifndef PD_PICOCALC_LCD_RST
#define PD_PICOCALC_LCD_RST 15
#endif
#ifndef PD_PICOCALC_LCD_HZ
#define PD_PICOCALC_LCD_HZ 25000000
#endif
#ifndef PD_PICOCALC_LCD_INVERT
#define PD_PICOCALC_LCD_INVERT 1   // 0 if colours look negative on your unit
#endif
#ifndef PD_PICOCALC_LCD_MADCTL
#define PD_PICOCALC_LCD_MADCTL 0x48
#endif

// ---- keyboard + battery: STM32 on I2C1
#ifndef PD_PICOCALC_KBD_I2C
#define PD_PICOCALC_KBD_I2C i2c1
#endif
#ifndef PD_PICOCALC_KBD_SDA
#define PD_PICOCALC_KBD_SDA 6
#endif
#ifndef PD_PICOCALC_KBD_SCL
#define PD_PICOCALC_KBD_SCL 7
#endif
#ifndef PD_PICOCALC_KBD_HZ
#define PD_PICOCALC_KBD_HZ 10000
#endif
#ifndef PD_PICOCALC_KBD_GAP_US
#define PD_PICOCALC_KBD_GAP_US 16000
#endif

// ---- SD card on SPI0
#ifndef PD_PICOCALC_SD_SPI
#define PD_PICOCALC_SD_SPI spi0
#endif
#ifndef PD_PICOCALC_SD_SCK
#define PD_PICOCALC_SD_SCK 18
#endif
#ifndef PD_PICOCALC_SD_MOSI
#define PD_PICOCALC_SD_MOSI 19
#endif
#ifndef PD_PICOCALC_SD_MISO
#define PD_PICOCALC_SD_MISO 16
#endif
#ifndef PD_PICOCALC_SD_CS
#define PD_PICOCALC_SD_CS 17
#endif
#ifndef PD_PICOCALC_SD_DETECT
#define PD_PICOCALC_SD_DETECT 22
#endif
#ifndef PD_PICOCALC_SD_HZ
#define PD_PICOCALC_SD_HZ 25000000
#endif

// ---- audio: PWM
#ifndef PD_PICOCALC_AUDIO_L
#define PD_PICOCALC_AUDIO_L 28   // mainboard v2.0; older boards use 26
#endif
#ifndef PD_PICOCALC_AUDIO_R
#define PD_PICOCALC_AUDIO_R 27
#endif

namespace pd::picocalc {

// Direct access to the concrete drivers, for things the HAL does not cover
// (the LCD DMA channel, raw south-bridge registers...).
drivers::St7365p& lcd();
drivers::PicoCalcSouthBridge& southbridge();
drivers::SdSpi& sd();
drivers::PwmAudio& pwm_audio();

}  // namespace pd::picocalc
