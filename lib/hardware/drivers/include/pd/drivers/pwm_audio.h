// PicoPods driver - stereo PWM audio fed by DMA from a ring buffer (PicoBoy's
// audio path, with the sample rate as a parameter).
//
// The PWM carrier runs at the sample rate and its wrap event paces the DMA,
// so buffered() is an exact audio clock: games can sync to it (push a frame
// of samples, then wait until buffered() drops below a target).
//
// The resolution depends on clk_sys / sample_rate: 200 MHz / 32768 Hz gives
// ~12.5 bits. Start it after the system clock is set.
#pragma once
#include "pd/hal/audio.h"
#include "pico/types.h"

namespace pd::drivers {

class PwmAudio : public hal::AudioOut {
public:
    struct Config {
        uint pin_l = 28;   // PicoCalc mainboard v2.0 (older boards: 26)
        uint pin_r = 27;
        int volume = 60;
    };

    void configure(const Config& cfg) { cfg_ = cfg; volume_ = cfg.volume; }

    bool start(uint32_t sample_rate) override;
    void stop() override;
    uint32_t sample_rate() const override { return rate_; }
    void push(const int16_t* stereo, int frames) override;
    int buffered() const override;
    int capacity() const override { return kRing - 700; }
    void silence() override;
    void set_volume(int percent) override;
    int volume() const override { return volume_; }

private:
    static constexpr int kRingBits = 11;   // 2048 frames per channel
    static constexpr int kRing = 1 << kRingBits;
    static constexpr uint32_t kMask = kRing - 1;
    static constexpr uint32_t kDmaCount = 0x0FFFFFFF;   // re-armed long before it runs out

    uint32_t read_pos() const;
    uint32_t level(int32_t s) const;
    uint32_t word(uint32_t lvl, uint chan) const;
    void recompute_gain();
    void start_dma(int ch, uint32_t* ring, uint slice);

    Config cfg_;
    int ch_l_ = -1, ch_r_ = -1;
    uint slice_l_ = 0, slice_r_ = 0, chan_l_ = 0, chan_r_ = 0;
    bool same_slice_ = false;
    uint32_t top_ = 0, mid_ = 0, wpos_ = 0, rate_ = 0;
    int volume_ = 60;
    int32_t gain_ = 0;
};

}  // namespace pd::drivers
