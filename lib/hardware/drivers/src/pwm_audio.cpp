// PWM audio via DMA (from PicoBoy).
#include "pd/drivers/pwm_audio.h"

#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

namespace pd::drivers {

namespace {
// The DMA ring wrap needs the buffers aligned to their size.
uint32_t g_ring_l[1 << 11] __attribute__((aligned((1 << 11) * 4)));
uint32_t g_ring_r[1 << 11] __attribute__((aligned((1 << 11) * 4)));
}  // namespace

void PwmAudio::recompute_gain() { gain_ = int32_t(int64_t(mid_) * volume_ / 100); }

uint32_t PwmAudio::word(uint32_t lvl, uint chan) const { return chan == PWM_CHAN_B ? (lvl << 16) : lvl; }

void PwmAudio::start_dma(int ch, uint32_t* ring, uint slice) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, kRingBits + 2);   // wrap the read address every kRing words
    channel_config_set_dreq(&c, pwm_get_dreq(slice));
    dma_channel_configure(ch, &c, &pwm_hw->slice[slice].cc, ring, kDmaCount, false);
}

bool PwmAudio::start(uint32_t sample_rate) {
    if (sample_rate < 4000 || sample_rate > 96000) return false;
    stop();
    slice_l_ = pwm_gpio_to_slice_num(cfg_.pin_l);
    chan_l_ = pwm_gpio_to_channel(cfg_.pin_l);
    slice_r_ = pwm_gpio_to_slice_num(cfg_.pin_r);
    chan_r_ = pwm_gpio_to_channel(cfg_.pin_r);
    same_slice_ = slice_l_ == slice_r_;

    const uint32_t sys = clock_get_hz(clk_sys);
    top_ = sys / sample_rate - 1;
    rate_ = sys / (top_ + 1);
    mid_ = top_ / 2;
    recompute_gain();

    gpio_set_function(cfg_.pin_l, GPIO_FUNC_PWM);
    gpio_set_function(cfg_.pin_r, GPIO_FUNC_PWM);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pc, 1);
    pwm_config_set_wrap(&pc, top_);
    pwm_init(slice_l_, &pc, false);
    if (!same_slice_) pwm_init(slice_r_, &pc, false);

    wpos_ = 0;
    silence();

    ch_l_ = dma_claim_unused_channel(true);
    start_dma(ch_l_, g_ring_l, slice_l_);
    uint32_t mask = 1u << ch_l_;
    if (!same_slice_) {
        ch_r_ = dma_claim_unused_channel(true);
        start_dma(ch_r_, g_ring_r, slice_r_);
        mask |= 1u << ch_r_;
    }
    dma_start_channel_mask(mask);
    pwm_set_mask_enabled((1u << slice_l_) | (1u << slice_r_));
    return true;
}

void PwmAudio::stop() {
    if (ch_l_ < 0) return;
    pwm_set_enabled(slice_l_, false);
    if (!same_slice_) pwm_set_enabled(slice_r_, false);
    for (int ch : {ch_l_, ch_r_}) {
        if (ch < 0) continue;
        dma_channel_abort(uint(ch));
        dma_channel_unclaim(uint(ch));
    }
    ch_l_ = ch_r_ = -1;
    rate_ = 0;
}

uint32_t PwmAudio::read_pos() const {
    if (ch_l_ < 0) return wpos_;
    const uintptr_t ra = dma_hw->ch[ch_l_].read_addr;
    return uint32_t((ra - uintptr_t(g_ring_l)) >> 2) & kMask;
}

int PwmAudio::buffered() const { return int((wpos_ - read_pos()) & kMask); }

void PwmAudio::silence() {
    for (int i = 0; i < kRing; ++i) {
        if (same_slice_) g_ring_l[i] = word(mid_, chan_l_) | word(mid_, chan_r_);
        else {
            g_ring_l[i] = word(mid_, chan_l_);
            g_ring_r[i] = word(mid_, chan_r_);
        }
    }
    if (ch_l_ >= 0) wpos_ = (read_pos() + 256) & kMask;   // restart with a small lead
}

uint32_t PwmAudio::level(int32_t s) const {
    int32_t v = int32_t(mid_) + ((s * gain_) >> 15);
    if (v < 0) v = 0;
    if (v > int32_t(top_)) v = int32_t(top_);
    return uint32_t(v);
}

void PwmAudio::push(const int16_t* st, int n) {
    if (ch_l_ < 0) return;
    // Re-arm the DMA if the (very long) transfer count ever ran out.
    if (!dma_channel_is_busy(ch_l_)) {
        dma_channel_set_trans_count(ch_l_, kDmaCount, false);
        uint32_t mask = 1u << ch_l_;
        if (!same_slice_) {
            dma_channel_set_trans_count(ch_r_, kDmaCount, false);
            mask |= 1u << ch_r_;
        }
        dma_start_channel_mask(mask);
    }
    // If the reader overtook us (underrun), jump ahead of it.
    if (buffered() > kRing - 700) wpos_ = (read_pos() + 256) & kMask;

    for (int i = 0; i < n; ++i) {
        const uint32_t l = level(st[i * 2]), r = level(st[i * 2 + 1]);
        if (same_slice_) g_ring_l[wpos_] = word(l, chan_l_) | word(r, chan_r_);
        else {
            g_ring_l[wpos_] = word(l, chan_l_);
            g_ring_r[wpos_] = word(r, chan_r_);
        }
        wpos_ = (wpos_ + 1) & kMask;
    }
}

void PwmAudio::set_volume(int percent) {
    volume_ = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    recompute_gain();
}

}  // namespace pd::drivers
