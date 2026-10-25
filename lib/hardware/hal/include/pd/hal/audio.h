// PicoPods HAL - a stereo audio output fed with 16-bit samples.
#pragma once
#include <cstdint>

namespace pd::hal {

class AudioOut {
public:
    virtual ~AudioOut() = default;
    // Starts output at `sample_rate` Hz (output is silent until push()).
    // Call again to change the rate. The rate may be rounded by the hardware;
    // sample_rate() reports the real one.
    virtual bool start(uint32_t sample_rate) = 0;
    virtual void stop() = 0;
    virtual uint32_t sample_rate() const = 0;

    // Queues `frames` interleaved L,R pairs. Drops what does not fit.
    virtual void push(const int16_t* stereo, int frames) = 0;
    // Frames queued but not yet played. Games pace themselves on this.
    virtual int buffered() const = 0;
    virtual int capacity() const = 0;
    // Discards the queue and outputs silence.
    virtual void silence() = 0;

    virtual void set_volume(int percent) = 0;  // 0..100
    virtual int volume() const = 0;
};

}  // namespace pd::hal
