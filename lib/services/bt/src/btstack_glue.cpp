// pd_bt: BTstack start-up for the CYW43, replacing the Pico SDK's
// btstack_cyw43.c. Identical except for where pairing keys live: the SDK
// keeps them in the last two sectors of flash (and erases them on first
// use), which on a PicoCalc may belong to the UF2 Loader's app area. Here
// they live in RAM, in the same format, and are saved to a file on the SD
// card whenever BTstack changes them.
#include <cstring>
#include <string>

extern "C" {
#include "btstack.h"
#include "pico/btstack_run_loop_async_context.h"
#include "btstack_tlv_flash_bank.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hal_flash_bank_memory.h"
#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_cyw43.h"
}

#include "btstack_modules.h"
#include "pd/fs.h"

namespace pd::bt::detail {

void on_stack_deinit();   // bt_cyw43.cpp: set everything up again on the next start

std::string g_key_file = "/.picopods/bt_keys.bin";
bool g_keys_dirty = false;

namespace {
constexpr uint32_t kBankSize = 4096;
uint8_t g_storage[2 * kBankSize];
hal_flash_bank_memory_t g_mem;
const hal_flash_bank_t* g_mem_impl = nullptr;
hal_flash_bank_t g_wrapped;
btstack_tlv_flash_bank_t g_tlv;

// Pass-through to the RAM banks, marking them for saving on changes.
uint32_t w_get_size(void* c) { return g_mem_impl->get_size(c); }
uint32_t w_get_alignment(void* c) { return g_mem_impl->get_alignment(c); }
void w_erase(void* c, int bank) {
    g_mem_impl->erase(c, bank);
    g_keys_dirty = true;
}
void w_read(void* c, int bank, uint32_t off, uint8_t* buf, uint32_t size) { g_mem_impl->read(c, bank, off, buf, size); }
void w_write(void* c, int bank, uint32_t off, const uint8_t* data, uint32_t size) {
    g_mem_impl->write(c, bank, off, data, size);
    g_keys_dirty = true;
}

void setup_tlv() {
    g_mem_impl = hal_flash_bank_memory_init_instance(&g_mem, g_storage, sizeof g_storage);   // fills 0xFF
    // Then the saved banks, if any.
    std::string saved;
    if (fs::mounted() && fs::read_file(g_key_file.c_str(), saved) && saved.size() == sizeof g_storage)
        memcpy(g_storage, saved.data(), sizeof g_storage);
    g_wrapped = *g_mem_impl;
    g_wrapped.get_size = w_get_size;
    g_wrapped.get_alignment = w_get_alignment;
    g_wrapped.erase = w_erase;
    g_wrapped.read = w_read;
    g_wrapped.write = w_write;
    g_keys_dirty = false;
    const btstack_tlv_t* tlv = btstack_tlv_flash_bank_init_instance(&g_tlv, &g_wrapped, &g_mem);
    btstack_tlv_set_instance(tlv, &g_tlv);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &g_tlv));
}
}  // namespace

// Saves the banks when BTstack changed them (called from pd::bt::poll()).
void save_keys_if_changed() {
    if (!g_keys_dirty || !fs::mounted()) return;
    g_keys_dirty = false;
    const std::string dir = g_key_file.substr(0, g_key_file.find_last_of('/'));
    if (!dir.empty()) fs::mkdirs(dir.c_str());
    fs::write_file(g_key_file.c_str(), std::string(reinterpret_cast<const char*>(g_storage), sizeof g_storage), true);
}

}  // namespace pd::bt::detail

extern "C" bool btstack_cyw43_init(async_context_t* context) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
    hci_init(hci_transport_cyw43_instance(), NULL);
    pd::bt::detail::setup_tlv();
    return true;
}

extern "C" void btstack_cyw43_deinit(async_context_t* context) {
    (void)context;
    pd::bt::detail::save_keys_if_changed();
    hci_power_control(HCI_POWER_OFF);
    hci_close();   // also frees the HCI state (do not call hci_deinit() after it)
    // The SDK's version stops here and frees all BTstack memory, leaving the
    // modules' lists pointing at it: the next start then crashes (HardFault in
    // l2cap_register_service). Reset them first.
    pd::bt::detail::btstack_modules_deinit();
    pd::bt::detail::on_stack_deinit();
    btstack_run_loop_deinit();
    btstack_memory_deinit();
}
