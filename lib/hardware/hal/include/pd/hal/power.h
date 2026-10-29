// PicoPods HAL - battery and power information.
#pragma once

namespace pd::hal {

class Power {
public:
    virtual ~Power() = default;
    virtual int battery_percent() = 0;   // 0..100, or -1 if unknown
    virtual bool charging() = 0;
};

}  // namespace pd::hal
