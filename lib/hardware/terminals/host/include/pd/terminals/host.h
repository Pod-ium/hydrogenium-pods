// PicoPods terminal - "host": runs PicoPods apps on a PC, headless.
//
// The display is an RGB framebuffer that can be written out as PPM images, the
// keyboard plays a script, the SD card is a folder, audio is discarded (but
// paced in real time, so games that sync to audio run at normal speed).
//
// Environment:
//   PD_KEYS    key script, comma separated (default: read tokens from stdin)
//              tokens: up down left right enter esc bksp tab space del ins home
//              end pgup pgdn f1..f10 comma, a single character ("a", "A", "?"),
//              0xNN, ctrl+x / shift+x / alt+x, text:hello (types the text),
//              wait:250 (ms), shot (dump a frame now)
//              set but empty: no key ever arrives (for tests that inject keys)
//   PD_FRAMES  folder for frame dumps: one PPM each time a key token starts,
//              and one at exit (default: none)
//   PD_SDCARD  folder used as the SD card root (default: ./sdcard)
//   PD_SCREEN  WxH (default: 320x320)
// When the script runs out the program exits with status 0.
//
// Interactive window (builds with SDL2, -DPD_HOST_SDL=ON or found by AUTO):
// pods open a window with live keyboard and real audio when PD_KEYS is not
// set, or when PD_WINDOW=1 (then a PD_KEYS script plays first and the window
// stays open after it). PD_WINDOW=0 forces the headless mode above.
//   PD_SCALE   window scale (default: 2)
// F12 saves a screenshot (PPM, in PD_FRAMES or the current folder).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "pd/terminal.h"

namespace pd::host {

bool dump_frame(const std::string& path);   // writes the framebuffer as PPM
const uint8_t* framebuffer();                // RGB888, width*height*3
// Pixel at (x, y) as 0xRRGGBB (tests).
Color pixel(int x, int y);
// Number of stereo frames the audio output has received.
uint64_t audio_frames_played();

// ---- for the interactive front end (host_window.cpp); all thread safe.
// Live key events, read before the key script. Interactive mode also stops
// reading stdin and keeps the program running when the script ends.
void push_key(hal::RawKey ev);
void set_interactive(bool on);
// Bumps on every display write; copy_frame() snapshots the framebuffer.
uint32_t frame_generation();
bool copy_frame(std::vector<uint8_t>& rgb, int& w, int& h);
// Where the audio output plays when set (otherwise it is only paced).
struct AudioSink {
    virtual ~AudioSink() = default;
    virtual bool open(uint32_t rate) = 0;
    virtual void close() = 0;
    virtual void queue(const int16_t* stereo, int frames) = 0;
    virtual int queued() = 0;   // frames not yet played
    virtual void clear() = 0;
};
void set_audio_sink(AudioSink* sink);

}  // namespace pd::host
