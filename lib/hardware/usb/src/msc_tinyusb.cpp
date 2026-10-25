// pd_usb_drive backend: TinyUSB mass-storage device on the RP2040/RP2350
// native USB port, one LUN backed by a hal::BlockDevice.
#include <cstring>

#include "msc_backend.h"
#include "pd/sys.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/usb.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"

// USB IDs. 0x1209/0x0001 is the pid.codes test ID; mass storage needs no
// vendor driver, so any ID works. Override for a product of your own.
#ifndef PD_USB_VID
#define PD_USB_VID 0x1209
#endif
#ifndef PD_USB_PID
#define PD_USB_PID 0x0001
#endif

namespace pd::usb::detail {

namespace {

hal::BlockDevice* g_dev = nullptr;
DriveOptions g_opt;
bool g_inited = false;
bool g_active = false;
volatile bool g_ejected = false;
volatile bool g_error = false;
uint64_t g_read = 0, g_written = 0;
uint32_t g_activity = 0;
uint8_t g_bounce[512];   // for transfers that do not start on a sector boundary

// The USB stack runs in the background, like the Pico SDK's USB serial: a
// low-priority software interrupt runs tud_task(), kicked every millisecond
// by a timer and right after every USB controller interrupt. The screen loop,
// keyboard reads (blocking I2C) and dialogs can then never delay the replies
// the computer is waiting for. The SD card callbacks run there too; the app
// does not touch the card while the mode is on, and the display is on
// another SPI bus.
int g_task_irq = -1;
repeating_timer_t g_timer;
bool g_timer_on = false;

volatile uint32_t g_irqs = 0, g_tasks = 0;
volatile uint32_t g_dev_desc = 0, g_cfg_desc = 0, g_str_desc = 0;

void task_irq_handler() {
    tud_task();
    g_tasks = g_tasks + 1;
}

// Runs after TinyUSB's own handler for every USB controller interrupt.
void usb_irq_counter() {
    g_irqs = g_irqs + 1;
    if (g_task_irq >= 0) irq_set_pending(uint(g_task_irq));
}

bool timer_cb(repeating_timer_t*) {
    if (g_task_irq >= 0) irq_set_pending(uint(g_task_irq));
    return true;
}

void background_on() {
    if (g_task_irq < 0) {
        g_task_irq = user_irq_claim_unused(true);
        irq_set_exclusive_handler(uint(g_task_irq), task_irq_handler);
        irq_add_shared_handler(USBCTRL_IRQ, usb_irq_counter, PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY);
    }
    irq_set_enabled(uint(g_task_irq), true);
    if (!g_timer_on) g_timer_on = add_repeating_timer_us(-1000, timer_cb, nullptr, &g_timer);
}

void background_off() {
    if (g_timer_on) {
        cancel_repeating_timer(&g_timer);
        g_timer_on = false;
    }
    if (g_task_irq >= 0) irq_set_enabled(uint(g_task_irq), false);
}

bool media_ok() { return g_dev && !g_ejected && g_dev->present() && g_dev->ready(); }

void copy_padded(uint8_t* dst, size_t n, const char* s) {
    memset(dst, ' ', n);
    const size_t l = strlen(s);
    memcpy(dst, s, l < n ? l : n);
}

}  // namespace

bool available() { return true; }

bool start(hal::BlockDevice* dev, const DriveOptions& opt) {
    g_dev = dev;
    g_opt = opt;
    g_ejected = false;
    g_error = false;
    g_read = g_written = 0;
    g_activity = 0;
    g_irqs = g_tasks = 0;
    g_dev_desc = g_cfg_desc = g_str_desc = 0;
    if (!g_inited) {
        if (!tusb_init()) return false;   // attaches to the bus
        g_inited = true;
    } else {
        tud_connect();
    }
    g_active = true;
    background_on();
    return true;
}

void task() {}

void stop() {
    if (!g_inited) return;
    // Let pending replies (the answer to "eject", for one) go out first; the
    // background task keeps running meanwhile.
    sys::sleep_ms(50);
    tud_disconnect();
    sys::sleep_ms(20);
    background_off();   // from here on nothing else touches the card
    g_active = false;
    if (g_dev) g_dev->sync();
}

Status status() {
    Status s;
    s.configured = g_active && tud_mounted();
    s.suspended = g_active && tud_suspended();
    s.ejected = g_ejected;
    s.media = media_ok();
    s.error = g_error;
    s.read_bytes = g_read;
    s.written_bytes = g_written;
    s.last_activity_ms = g_activity;
    s.irqs = g_irqs;
    s.dev_desc = g_dev_desc;
    s.cfg_desc = g_cfg_desc;
    s.str_desc = g_str_desc;
    s.tasks = g_tasks;
    s.address = uint8_t(usb_hw->dev_addr_ctrl & 0x7F);
    s.usb_clk_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    s.pullup = (usb_hw->sie_ctrl & USB_SIE_CTRL_PULLUP_EN_BITS) != 0;
    return s;
}

}  // namespace pd::usb::detail

// ==================================================================== TinyUSB
using namespace pd::usb::detail;

namespace {

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
constexpr uint8_t EP_MSC_OUT = 0x01;
constexpr uint8_t EP_MSC_IN = 0x81;
constexpr int CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

const tusb_desc_device_t kDeviceDesc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,   // class per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PD_USB_VID,
    .idProduct = PD_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t kConfigDesc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EP_MSC_OUT, EP_MSC_IN, 64),
};

uint16_t g_str[33];

uint32_t now() { return pd::sys::now_ms(); }

bool sector_range_ok(uint32_t lba, uint32_t bytes) {
    const uint32_t count = g_dev ? g_dev->sector_count() : 0;
    const uint32_t sectors = (bytes + 511) / 512;
    return count == 0 || (lba < count && sectors <= count - lba);
}

void not_ready_sense(uint8_t lun) {
    // NOT READY / MEDIUM NOT PRESENT
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
}

}  // namespace

extern "C" {

uint8_t const* tud_descriptor_device_cb(void) {
    g_dev_desc = g_dev_desc + 1;
    return reinterpret_cast<const uint8_t*>(&kDeviceDesc); }

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    g_cfg_desc = g_cfg_desc + 1;
    return kConfigDesc;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    g_str_desc = g_str_desc + 1;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char* s = nullptr;
    size_t n = 0;
    switch (index) {
        case 0:
            g_str[1] = 0x0409;   // English (US)
            n = 1;
            break;
        case 1: s = g_opt.manufacturer; break;
        case 2: s = g_opt.usb_product; break;
        case 3:
            pico_get_unique_board_id_string(serial, sizeof serial);
            s = serial;
            break;
        default: return nullptr;
    }
    if (s) {
        n = strlen(s);
        if (n > 32) n = 32;
        for (size_t i = 0; i < n; ++i) g_str[1 + i] = uint8_t(s[i]);
    }
    g_str[0] = uint16_t((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return g_str;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    copy_padded(vendor_id, 8, g_opt.vendor);
    copy_padded(product_id, 16, g_opt.product);
    copy_padded(product_rev, 4, "1.0");
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (media_ok()) return true;
    not_ready_sense(lun);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_count = media_ok() ? g_dev->sector_count() : 0;
    *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {   // "eject" from the computer
        if (g_dev) g_dev->sync();
        g_ejected = true;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return !g_opt.read_only;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!media_ok()) {
        not_ready_sense(lun);
        return -1;
    }
    lba += offset / 512;
    offset %= 512;
    if (!sector_range_ok(lba, offset + bufsize)) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);   // LBA out of range
        return -1;
    }
    uint8_t* out = static_cast<uint8_t*>(buffer);
    int32_t done = 0;
    if (offset == 0 && bufsize >= 512) {
        const uint32_t sectors = bufsize / 512;
        if (!g_dev->read(lba, out, sectors)) goto fail;
        done = int32_t(sectors * 512);
    } else {   // partial sector (not expected with 512-byte blocks)
        if (!g_dev->read(lba, g_bounce, 1)) goto fail;
        const uint32_t n = (512 - offset) < bufsize ? (512 - offset) : bufsize;
        memcpy(out, g_bounce + offset, n);
        done = int32_t(n);
    }
    g_read += uint32_t(done);
    g_activity = now();
    return done;
fail:
    g_error = true;
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // unrecovered read error
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (g_opt.read_only) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);   // write protected
        return -1;
    }
    if (!media_ok()) {
        not_ready_sense(lun);
        return -1;
    }
    lba += offset / 512;
    offset %= 512;
    if (!sector_range_ok(lba, offset + bufsize)) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
        return -1;
    }
    int32_t done = 0;
    if (offset == 0 && bufsize >= 512) {
        const uint32_t sectors = bufsize / 512;
        if (!g_dev->write(lba, buffer, sectors)) goto fail;
        done = int32_t(sectors * 512);
    } else {   // read-modify-write of one sector
        if (!g_dev->read(lba, g_bounce, 1)) goto fail;
        const uint32_t n = (512 - offset) < bufsize ? (512 - offset) : bufsize;
        memcpy(g_bounce + offset, buffer, n);
        if (!g_dev->write(lba, g_bounce, 1)) goto fail;
        done = int32_t(n);
    }
    g_written += uint32_t(done);
    g_activity = now();
    return done;
fail:
    g_error = true;
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);   // write error
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
        case 0x35:   // SYNCHRONIZE CACHE (10): writes are already on the card
            if (g_dev) g_dev->sync();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);   // invalid command
            return -1;
    }
}

}  // extern "C"
