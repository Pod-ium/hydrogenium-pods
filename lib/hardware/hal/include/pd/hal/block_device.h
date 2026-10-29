// PicoPods HAL - a 512-byte-sector block device (SD card, flash disk...).
// pd::fs mounts FatFs on it.
#pragma once
#include <cstdint>

namespace pd::hal {

class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual bool init() = 0;          // (re)initialises the medium
    virtual bool ready() const = 0;   // init() succeeded and nothing failed since
    virtual bool present() { return true; }   // card-detect switch, if any
    virtual bool read(uint32_t sector, uint8_t* buf, uint32_t count) = 0;
    virtual bool write(uint32_t sector, const uint8_t* buf, uint32_t count) = 0;
    virtual bool sync() { return true; }
    virtual uint32_t sector_count() const = 0;   // 0 if unknown
};

}  // namespace pd::hal
