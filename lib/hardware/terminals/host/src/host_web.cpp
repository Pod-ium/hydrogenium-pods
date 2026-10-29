// PicoPods terminal - host: browser front end (Emscripten builds).
//
// Linked into pods built with emcmake. The pod's own main() runs on the
// page's thread; pd::sys yields to the browser from its clock and sleeps
// (Asyncify), and in between the page (web/run.js) draws the framebuffer,
// feeds keys from the touch or PC keyboard, and plays the audio queued here.
// The SD card is /sd, an IndexedDB-backed folder the page mounts before main().
#include <emscripten.h>

#include <cstdint>
#include <cstdlib>

#include "pd/terminals/host.h"

namespace {

// Audio goes straight to Web Audio: each push becomes a buffer scheduled
// right after the previous one (web/run.js, Module.pdAudio).
class WebAudio : public pd::host::AudioSink {
public:
    bool open(uint32_t rate) override {
        return EM_ASM_INT({ return Module.pdAudio ? Module.pdAudio.open($0) : 0; }, rate) != 0;
    }
    void close() override {
        EM_ASM({ if (Module.pdAudio) Module.pdAudio.close(); });
    }
    void queue(const int16_t* stereo, int frames) override {
        EM_ASM({ if (Module.pdAudio) Module.pdAudio.queue($0, $1); }, stereo, frames);
    }
    int queued() override {
        return EM_ASM_INT({ return Module.pdAudio ? Module.pdAudio.queued() : 0; });
    }
    void clear() override {
        EM_ASM({ if (Module.pdAudio) Module.pdAudio.clear(); });
    }
};

WebAudio g_audio;

// Before main(): the card folder, live keys only, audio through the page.
struct Setup {
    Setup() {
        setenv("PD_SDCARD", "/sd", 0);
        pd::host::set_interactive(true);
        pd::host::set_audio_sink(&g_audio);
    }
} g_setup;

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void pd_web_key(int code, int state) {
    pd::host::push_key(pd::hal::RawKey{uint8_t(code), uint8_t(state)});
}

// RGB888 framebuffer (nullptr until the pod set up its terminal).
EMSCRIPTEN_KEEPALIVE const uint8_t* pd_web_frame() { return pd::host::framebuffer(); }
EMSCRIPTEN_KEEPALIVE uint32_t pd_web_frame_generation() { return pd::host::frame_generation(); }
EMSCRIPTEN_KEEPALIVE int pd_web_width() { return pd::terminal().display_ ? pd::terminal().display().width() : 0; }
EMSCRIPTEN_KEEPALIVE int pd_web_height() { return pd::terminal().display_ ? pd::terminal().display().height() : 0; }

}  // extern "C"
