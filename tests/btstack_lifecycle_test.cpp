// Host test: BTstack's real core (HCI, L2CAP, RFCOMM, SDP, GOEP) started and
// stopped several times the way pd_bt does on the device (radio off = full
// BTstack teardown), with a dummy radio, under AddressSanitizer.
//   btstack_lifecycle_test         pd_bt's teardown: must run clean
//   btstack_lifecycle_test --old   the Pico SDK's teardown: ASan must catch
//                                  the use-after-free (the HardFault seen on
//                                  the PicoCalc in l2cap_register_service)
#include <cstdio>
#include <cstring>
#include <vector>

#include "../lib/services/bt/src/btstack_modules.h"

extern "C" {
#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "classic/sdp_util.h"
#include "bluetooth_sdp.h"
#include "classic/goep_client.h"
#include "classic/goep_server.h"
#include "hci_transport.h"
}

namespace {
void t_init(const void*) {}
int t_open() { return 0; }
int t_close() { return 0; }
void t_register(void (*)(uint8_t, uint8_t*, uint16_t)) {}
int t_can_send(uint8_t) { return 1; }
int t_send(uint8_t, uint8_t*, int) { return 0; }
hci_transport_t g_dummy;

void handler(uint8_t, uint16_t, uint8_t*, uint16_t) {}
btstack_packet_callback_registration_t g_reg;
uint8_t g_record[160];

// What pd_bt's start() does (setup_stack()).
void start_stack() {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_init(&g_dummy, nullptr);
    l2cap_init();
    sdp_init();
    rfcomm_init();
    goep_server_init();
    goep_client_init();
    gap_set_local_name("PicoCalc 00:00:00:00:00:00");
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    g_reg.callback = &handler;
    hci_add_event_handler(&g_reg);
    memset(g_record, 0, sizeof g_record);
    de_create_sequence(g_record);
    de_add_number(g_record, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(g_record, DE_UINT, DE_SIZE_32, sdp_create_service_record_handle());
    sdp_register_service(g_record);
    const uint8_t st = goep_server_register_service(&handler, 12, 0xFFFF, 0, 0, LEVEL_2);
    if (st) {
        fprintf(stderr, "goep_server_register_service failed: 0x%02x\n", st);
        exit(1);
    }
}

// btstack_cyw43_deinit(): the SDK's, or pd_bt's.
void stop_stack(bool old) {
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    if (!old) pd::bt::detail::btstack_modules_deinit();
    btstack_run_loop_deinit();
    btstack_memory_deinit();
}
}  // namespace

// The Object Push SDP record, read back the way a phone does.
static uint16_t uuid16_of(const uint8_t* el) {   // 16-bit UUID element: 0x19 hi lo
    return el[0] == 0x19 ? uint16_t((el[1] << 8) | el[2]) : 0;
}
static int check_sdp_record() {
    uint8_t rec[160];
    const unsigned len = pd::bt::detail::build_opp_sdp_record(rec, sizeof rec, 0x10001, 12);
    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            fprintf(stderr, "[sdp] FAILED: %s\n", what);
            ++fails;
        }
    };
    expect(len > 20 && len <= sizeof rec, "record length");
    expect(de_get_len(rec) == len, "declared length matches");
    // service class: Object Push
    uint8_t* cls = sdp_get_attribute_value_for_attribute_id(rec, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    bool opp = false;
    des_iterator_t it;
    if (cls && des_iterator_init(&it, cls))
        for (; des_iterator_has_more(&it); des_iterator_next(&it))
            opp = opp || uuid16_of(des_iterator_get_element(&it)) == BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH;
    expect(opp, "service class OBEX Object Push");
    // protocols: L2CAP, RFCOMM channel, OBEX - in this order
    uint8_t* pd_list = sdp_get_attribute_value_for_attribute_id(rec, BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    std::vector<uint16_t> protos;
    int channel = -1;
    if (pd_list && des_iterator_init(&it, pd_list)) {
        for (; des_iterator_has_more(&it); des_iterator_next(&it)) {
            des_iterator_t inner;
            if (!des_iterator_init(&inner, des_iterator_get_element(&it))) continue;
            const uint16_t u = uuid16_of(des_iterator_get_element(&inner));
            protos.push_back(u);
            des_iterator_next(&inner);
            if (u == BLUETOOTH_PROTOCOL_RFCOMM && des_iterator_has_more(&inner)) {
                const uint8_t* el = des_iterator_get_element(&inner);
                if (el[0] == 0x08) channel = el[1];   // UINT8
            }
        }
    }
    expect(protos.size() == 3 && protos[0] == BLUETOOTH_PROTOCOL_L2CAP && protos[1] == BLUETOOTH_PROTOCOL_RFCOMM &&
               protos[2] == BLUETOOTH_PROTOCOL_OBEX,
           "protocols L2CAP / RFCOMM / OBEX");
    expect(channel == 12, "RFCOMM channel 12");
    // profile: OPP 1.2
    uint8_t* prof = sdp_get_attribute_value_for_attribute_id(rec, BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    bool v12 = false;
    if (prof && des_iterator_init(&it, prof)) {
        des_iterator_t inner;
        if (des_iterator_init(&inner, des_iterator_get_element(&it)) &&
            uuid16_of(des_iterator_get_element(&inner)) == BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH) {
            des_iterator_next(&inner);
            uint16_t ver = 0;
            v12 = des_iterator_has_more(&inner) && de_element_get_uint16(des_iterator_get_element(&inner), &ver) && ver == 0x0102;
        }
    }
    expect(v12, "profile OPP 1.2");
    expect(sdp_get_attribute_value_for_attribute_id(rec, BLUETOOTH_ATTRIBUTE_SUPPORTED_FORMATS_LIST) != nullptr,
           "supported formats list");
    fprintf(stderr, "[sdp] record %u bytes, channel %d: %s\n", len, channel, fails ? "BAD" : "ok");
    return fails;
}

int main(int argc, char** argv) {
    if (check_sdp_record()) return 1;
    const bool old = argc > 1 && strcmp(argv[1], "--old") == 0;
    memset(&g_dummy, 0, sizeof g_dummy);
    g_dummy.name = "dummy";
    g_dummy.init = t_init;
    g_dummy.open = t_open;
    g_dummy.close = t_close;
    g_dummy.register_packet_handler = t_register;
    g_dummy.can_send_packet_now = t_can_send;
    g_dummy.send_packet = t_send;
    for (int round = 1; round <= 3; ++round) {
        fprintf(stderr, "[btstack] start %d\n", round);
        start_stack();
        stop_stack(old);
    }
    fprintf(stderr, "[btstack] 3 start/stop cycles clean\n");
    return 0;
}
