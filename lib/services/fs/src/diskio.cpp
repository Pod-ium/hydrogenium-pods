// FatFs disk I/O glue: routes FatFs to the hal::BlockDevice set with
// pd::fs::set_block_device() (the terminal does that during init).
#include "pd/hal/block_device.h"

extern "C" {
#include "ff.h"   // before diskio.h: it defines BYTE, LBA_t...
#include "diskio.h"
}

namespace pd::fs {
extern hal::BlockDevice* g_block_device;
}

using pd::fs::g_block_device;

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && g_block_device && g_block_device->ready()) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0 || !g_block_device) return STA_NOINIT;
    if (!g_block_device->present()) return STA_NOINIT | STA_NODISK;
    return g_block_device->init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_block_device || !g_block_device->ready()) return RES_NOTRDY;
    return g_block_device->read(uint32_t(sector), buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_block_device || !g_block_device->ready()) return RES_NOTRDY;
    return g_block_device->write(uint32_t(sector), buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0 || !g_block_device || !g_block_device->ready()) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC: return g_block_device->sync() ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = g_block_device->sector_count(); return RES_OK;
        case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
        case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
        default: return RES_PARERR;
    }
}

}  // extern "C"
