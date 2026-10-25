// PicoPods - flash cache on RP2040 / RP2350 (PicoBoy's romstore, generalised).
#include <cstring>

#include "hardware/flash.h"
#include "pd/board.h"
#include "pd/flash_cache.h"
#include "pd/fs.h"
#include "pd/uf2loader.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#if PD_CHIP_RP2350
#include "hardware/structs/qmi.h"
#endif

extern "C" char __flash_binary_end;

namespace pd::flash_cache {

namespace {
constexpr uint32_t kBlock = 65536;
constexpr uint32_t kMagic = 0x50444643;   // "PDFC"
constexpr uint32_t kHeadBytes = 512;      // bytes compared to recognise the file

struct Meta {
    uint32_t magic;
    uint32_t size;
    uint32_t mtime;
    uint32_t checksum;     // of the first kHeadBytes; guards against stale data
    char path[240];
};
static_assert(sizeof(Meta) <= FLASH_PAGE_SIZE, "meta must fit a page");

uint32_t g_phys_base = 0;   // physical flash offset that XIP offset 0 maps to
uint32_t g_data_off = 0;    // XIP offset of the data (64 KB aligned)
uint32_t g_meta_off = 0;    // XIP offset of the metadata sector
uint32_t g_capacity = 0;

inline const uint8_t* xip(uint32_t off) { return reinterpret_cast<const uint8_t*>(XIP_BASE + off); }

struct EraseArgs { uint32_t off, len; };
struct ProgArgs { uint32_t off; const uint8_t* data; uint32_t len; };
void do_erase(void* p) {
    auto* a = static_cast<EraseArgs*>(p);
    flash_range_erase(g_phys_base + a->off, a->len);
}
void do_prog(void* p) {
    auto* a = static_cast<ProgArgs*>(p);
    flash_range_program(g_phys_base + a->off, a->data, a->len);
}
bool erase(uint32_t off, uint32_t len) {
    EraseArgs a{off, len};
    return flash_safe_execute(do_erase, &a, 1000) == PICO_OK;
}
bool program(uint32_t off, const uint8_t* d, uint32_t len) {
    ProgArgs a{off, d, len};
    return flash_safe_execute(do_prog, &a, 1000) == PICO_OK;
}

uint32_t checksum(const uint8_t* p, uint32_t n) {
    uint32_t s = 2166136261u;   // FNV-1a
    for (uint32_t i = 0; i < n; ++i) s = (s ^ p[i]) * 16777619u;
    return s;
}

inline const uint8_t* fail(const char** err, const char* msg) {
    if (err) *err = msg;
    return nullptr;
}
}  // namespace

bool init(uint32_t max_bytes) {
    uint32_t visible = PICO_FLASH_SIZE_BYTES;
    g_phys_base = 0;
#if PD_CHIP_RP2350
    // With UF2 Loader on the RP2350, QMI address-translation window 0 maps XIP
    // offset 0 to the start of the app partition and limits its size. Without
    // a loader the window is the identity.
    const uint32_t at = qmi_hw->atrans[0];
    const uint32_t base_sect = (at & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB;
    const uint32_t size_sect = (at & QMI_ATRANS0_SIZE_BITS) >> QMI_ATRANS0_SIZE_LSB;
    g_phys_base = base_sect * FLASH_SECTOR_SIZE;
    visible = size_sect * FLASH_SECTOR_SIZE;
    if (visible == 0 || visible > PICO_FLASH_SIZE_BYTES - g_phys_base) visible = PICO_FLASH_SIZE_BYTES - g_phys_base;
#else
    // RP2040: UF2 Loader lives in the top of the flash and says, in the app's
    // vector table, where the app's usable flash ends (see pd/uf2loader.h).
    const auto* info = reinterpret_cast<const volatile uint32_t*>(XIP_BASE + uf2loader::kInfoOffset);
    visible = uf2loader::rp2040_app_flash_end(PICO_FLASH_SIZE_BYTES, info[0], info[1]);
#endif
    const uint32_t end = visible & ~(kBlock - 1);
    g_meta_off = end - FLASH_SECTOR_SIZE;
    const uint32_t bin_end =
        (uint32_t(uintptr_t(&__flash_binary_end)) - XIP_BASE + kBlock - 1) & ~(kBlock - 1);
    const uint32_t want = end > max_bytes + kBlock ? ((end - max_bytes - kBlock) & ~(kBlock - 1)) : 0;
    g_data_off = want > bin_end ? want : bin_end;
    g_capacity = g_meta_off > g_data_off ? g_meta_off - g_data_off : 0;
    return g_capacity > 0;
}

uint32_t capacity() { return g_capacity; }

void invalidate() {
    if (g_capacity) erase(g_meta_off, FLASH_SECTOR_SIZE);
}

const uint8_t* load(const char* path, uint32_t* size_out, const Progress& progress, const char** err) {
    static uint8_t buf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
    if (err) *err = nullptr;
    if (!g_capacity) return fail(err, "Flash cache not initialised");

    fs::Info info;
    if (!fs::stat(path, &info) || info.is_dir) return fail(err, "Cannot read file");
    const uint32_t size = info.size;
    if (size == 0) return fail(err, "File is empty");
    if (size > g_capacity) return fail(err, "File too large for the flash cache");

    fs::File f;
    if (!f.open(path, fs::Mode::Read)) return fail(err, "Cannot open file");
    const uint32_t head = size < kHeadBytes ? size : kHeadBytes;
    if (f.read(buf, head) != int(head)) return fail(err, "Read error");
    const uint32_t sum = checksum(buf, head);

    // Already cached?
    const Meta* m = reinterpret_cast<const Meta*>(xip(g_meta_off));
    if (m->magic == kMagic && m->size == size && m->mtime == info.mtime && m->checksum == sum &&
        strncmp(m->path, path, sizeof(m->path)) == 0 && checksum(xip(g_data_off), head) == sum) {
        if (size_out) *size_out = size;
        return xip(g_data_off);
    }

    // Invalidate first, so an interrupted copy is never mistaken for a valid cache.
    if (!erase(g_meta_off, FLASH_SECTOR_SIZE)) return fail(err, "Flash erase failed");
    f.seek(0);
    const uint32_t padded = (size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    for (uint32_t off = 0; off < padded; off += FLASH_SECTOR_SIZE) {
        if ((off % kBlock) == 0) {
            const uint32_t len = padded - off >= kBlock ? kBlock : padded - off;
            if (!erase(g_data_off + off, len)) return fail(err, "Flash erase failed");
        }
        memset(buf, 0xFF, sizeof buf);
        if (f.read(buf, FLASH_SECTOR_SIZE) < 0) return fail(err, "SD read error");
        if (!program(g_data_off + off, buf, FLASH_SECTOR_SIZE)) return fail(err, "Flash write failed");
        if (progress && !progress(off + FLASH_SECTOR_SIZE, padded)) return fail(err, "Cancelled");
    }
    f.close();

    static Meta nm __attribute__((aligned(4)));
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(&nm, 0, sizeof nm);
    nm.magic = kMagic;
    nm.size = size;
    nm.mtime = info.mtime;
    nm.checksum = checksum(xip(g_data_off), head);
    strncpy(nm.path, path, sizeof(nm.path) - 1);
    memset(page, 0xFF, sizeof page);
    memcpy(page, &nm, sizeof nm);
    if (!program(g_meta_off, page, FLASH_PAGE_SIZE)) return fail(err, "Flash write failed");

    if (size_out) *size_out = size;
    return xip(g_data_off);
}

}  // namespace pd::flash_cache
