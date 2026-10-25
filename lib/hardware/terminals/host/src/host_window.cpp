// PicoPods terminal - host: interactive SDL2 window (PD_HOST_SDL builds).
//
// Linked into pods only. The pod's own main() is renamed pd_host_app_main
// (compile definition main=pd_host_app_main, see picopods_add_pod()) so the
// window can own the main thread, which SDL needs on every platform; the pod
// runs on a second thread. Without a window (PD_KEYS set, PD_WINDOW=0, or no
// display) the pod runs on the main thread exactly as in a headless build.
#undef main

#define SDL_MAIN_HANDLED   // main() is ours: no SDL_main / SDL2main
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pd/keys.h"
#include "pd/terminals/host.h"

int pd_host_app_main();

namespace {

using pd::hal::RawKey;

// ---------------------------------------------------------------- audio
class SdlAudio : public pd::host::AudioSink {
public:
    bool open(uint32_t rate) override {
        close();
        SDL_AudioSpec want{}, have{};
        want.freq = int(rate);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);   // SDL converts
        if (!dev_) {
            fprintf(stderr, "[window] no audio: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(dev_, 0);
        return true;
    }
    void close() override {
        if (dev_) SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    void queue(const int16_t* stereo, int frames) override {
        if (dev_ && frames > 0) SDL_QueueAudio(dev_, stereo, Uint32(frames) * 4);
    }
    int queued() override { return dev_ ? int(SDL_GetQueuedAudioSize(dev_) / 4) : 0; }
    void clear() override {
        if (dev_) SDL_ClearQueuedAudio(dev_);
    }

private:
    SDL_AudioDeviceID dev_ = 0;
};

// ---------------------------------------------------------------- keyboard
// Keys that are not text map to their PicoCalc code; text keys arrive as the
// character they type (SDL_TEXTINPUT, so the PC's layout applies: Shift+a is
// 'A', like on the PicoCalc). The character is remembered per physical key
// so its release matches its press.
uint8_t special_key(SDL_Keycode k) {
    namespace key = pd::key;
    switch (k) {
        case SDLK_UP: return key::Up;
        case SDLK_DOWN: return key::Down;
        case SDLK_LEFT: return key::Left;
        case SDLK_RIGHT: return key::Right;
        case SDLK_RETURN: case SDLK_KP_ENTER: return key::Enter;
        case SDLK_ESCAPE: return key::Esc;
        case SDLK_BACKSPACE: return key::Backspace;
        case SDLK_TAB: return key::Tab;
        case SDLK_DELETE: return key::Del;
        case SDLK_INSERT: return key::Insert;
        case SDLK_HOME: return key::Home;
        case SDLK_END: return key::End;
        case SDLK_PAGEUP: return key::PageUp;
        case SDLK_PAGEDOWN: return key::PageDown;
        case SDLK_PAUSE: return key::Break;
        case SDLK_F1: return key::F1;
        case SDLK_F2: return key::F2;
        case SDLK_F3: return key::F3;
        case SDLK_F4: return key::F4;
        case SDLK_F5: return key::F5;
        case SDLK_F6: return key::F6;
        case SDLK_F7: return key::F7;
        case SDLK_F8: return key::F8;
        case SDLK_F9: return key::F9;
        case SDLK_F10: return key::F10;
        case SDLK_LSHIFT: return key::ShiftL;
        case SDLK_RSHIFT: return key::ShiftR;
        case SDLK_LCTRL: case SDLK_RCTRL: return key::Ctrl;
        case SDLK_LALT: return key::Alt;   // right Alt is AltGr: left to text input
        case SDLK_CAPSLOCK: return key::CapsLock;
        default: return 0;
    }
}

class Keys {
public:
    void down(const SDL_KeyboardEvent& e) {
        if (e.repeat) return;   // pd::input::Keyboard does its own auto-repeat
        if (const uint8_t c = special_key(e.keysym.sym)) {
            send(c, pd::hal::KEY_PRESSED);
            return;
        }
        const SDL_Keycode k = e.keysym.sym;
        if (k < 0x20 || k >= 0x7F) return;
        // With Ctrl or Alt held most platforms type no text: use the key itself.
        if (e.keysym.mod & (KMOD_CTRL | KMOD_LALT)) {
            press(e.keysym.scancode, uint8_t(k));
            return;
        }
        pending_ = e.keysym.scancode;   // its character follows as SDL_TEXTINPUT
    }
    void text(const char* t) {
        if (pending_ == SDL_SCANCODE_UNKNOWN) return;
        const unsigned char c = static_cast<unsigned char>(t[0]);
        if (c >= 0x20 && c < 0x7F && t[1] == 0) press(pending_, c);   // ASCII only, like the PicoCalc
        pending_ = SDL_SCANCODE_UNKNOWN;
    }
    void up(const SDL_KeyboardEvent& e) {
        if (const uint8_t c = special_key(e.keysym.sym)) {
            send(c, pd::hal::KEY_RELEASED);
            return;
        }
        uint8_t& c = held_[e.keysym.scancode];
        if (c) send(c, pd::hal::KEY_RELEASED);
        c = 0;
    }

private:
    void press(SDL_Scancode sc, uint8_t c) {
        held_[sc] = c;
        send(c, pd::hal::KEY_PRESSED);
    }
    static void send(uint8_t code, uint8_t state) { pd::host::push_key(RawKey{code, state}); }

    SDL_Scancode pending_ = SDL_SCANCODE_UNKNOWN;
    uint8_t held_[SDL_NUM_SCANCODES] = {};
};

// ---------------------------------------------------------------- window
void screenshot() {
    static int n = 0;
    const char* dir = getenv("PD_FRAMES");
    char name[64];
    snprintf(name, sizeof name, "screenshot%03d.ppm", n++);
    const std::string path = (dir && *dir ? std::string(dir) + "/" : std::string()) + name;
    if (pd::host::dump_frame(path)) fprintf(stderr, "[window] saved %s\n", path.c_str());
}

// Runs the pod behind a window; false (the pod not started) if there is none.
bool run_window(const char* title, int scale, int& code) {
    SDL_Window* win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       320 * scale, 320 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) {
        fprintf(stderr, "[window] %s, running headless\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SdlAudio audio;   // no sound card: the pod still runs, paced but silent
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
        pd::host::set_audio_sink(&audio);
    else
        fprintf(stderr, "[window] no audio: %s\n", SDL_GetError());
    pd::host::set_interactive(true);

    std::atomic<bool> done{false};
    std::thread app([&] {
        code = pd_host_app_main();
        done = true;
        SDL_Event quit{};
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    });

    Keys keys;
    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    uint32_t shown = ~0u;
    std::vector<uint8_t> rgb;
    SDL_StartTextInput();
    for (bool quit = false; !quit;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) {
            do {
                switch (e.type) {
                    case SDL_QUIT: quit = true; break;
                    case SDL_KEYDOWN:
                        if (e.key.keysym.sym == SDLK_F12) {
                            if (!e.key.repeat) screenshot();
                        } else {
                            keys.down(e.key);
                        }
                        break;
                    case SDL_KEYUP: keys.up(e.key); break;
                    case SDL_TEXTINPUT: keys.text(e.text.text); break;
                    case SDL_WINDOWEVENT: shown = ~0u; break;   // redraw after expose/resize
                }
            } while (SDL_PollEvent(&e));
        }
        const uint32_t gen = pd::host::frame_generation();
        if (gen == shown) continue;
        int w = 0, h = 0;
        if (!pd::host::copy_frame(rgb, w, h)) continue;
        shown = gen;
        if (!tex || w != tw || h != th) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
            tw = w;
            th = h;
            SDL_RenderSetLogicalSize(ren, w, h);   // keeps the aspect, letterboxed
        }
        SDL_UpdateTexture(tex, nullptr, rgb.data(), w * 3);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }

    if (!done) {
        // Window closed while the pod runs: leave without unwinding its thread
        // (static destructors would race it). Files it has closed are on disk.
        fflush(nullptr);
        std::_Exit(0);
    }
    app.join();
    pd::host::set_audio_sink(nullptr);
    audio.close();
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* want = getenv("PD_WINDOW");
    const bool window = want ? atoi(want) != 0 : getenv("PD_KEYS") == nullptr;
    if (window) {
        SDL_SetMainReady();
        // SDL falls back to its invisible "offscreen" driver when there is no
        // display: that is no window to anyone, unless asked for by name.
        const bool ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0;
        const char* drv = ok ? SDL_GetCurrentVideoDriver() : nullptr;
        if (ok && drv && !getenv("SDL_VIDEODRIVER") && (!strcmp(drv, "offscreen") || !strcmp(drv, "dummy"))) {
            fprintf(stderr, "[window] no display, running headless\n");
            SDL_Quit();
        } else if (ok) {
            const char* s = getenv("PD_SCALE");
            const int scale = s && atoi(s) > 0 ? atoi(s) : 2;
            std::string title = argc > 0 ? argv[0] : "PicoPods";
            title = "PicoPods - " + title.substr(title.find_last_of("/\\") + 1);
            int code = 0;
            const bool ran = run_window(title.c_str(), scale, code);
            SDL_Quit();
            if (ran) return code;
        } else {
            fprintf(stderr, "[window] no display (%s), running headless\n", SDL_GetError());
        }
    }
    return pd_host_app_main();
}
