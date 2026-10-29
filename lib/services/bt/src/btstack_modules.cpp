// pd_bt internal: see btstack_modules.h
#include "btstack_modules.h"

#include <cstring>

extern "C" {
#include "l2cap.h"
#include "classic/goep_client.h"
#include "classic/goep_server.h"
#include "classic/rfcomm.h"
#include "classic/sdp_client.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "bluetooth_sdp.h"
// goep_server.h declares it as "geop_server_deinit" (typo) while goep_server.c
// defines goep_server_deinit: declare the real one.
void goep_server_deinit(void);
}

namespace pd::bt::detail {

void btstack_modules_deinit() {
    goep_client_deinit();
    goep_server_deinit();
    rfcomm_deinit();
    sdp_client_deinit();
    sdp_deinit();
    l2cap_deinit();
}

unsigned build_opp_sdp_record(unsigned char* service, unsigned buf_len, unsigned long handle, unsigned char channel) {
    memset(service, 0, buf_len);
    de_create_sequence(service);
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(service, DE_UINT, DE_SIZE_32, uint32_t(handle));
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    uint8_t* attr = de_push_sequence(service);
    de_add_number(attr, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
    de_pop_sequence(service, attr);
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    attr = de_push_sequence(service);
    {
        uint8_t* l2 = de_push_sequence(attr);
        de_add_number(l2, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_L2CAP);
        de_pop_sequence(attr, l2);
        uint8_t* rf = de_push_sequence(attr);
        de_add_number(rf, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_RFCOMM);
        de_add_number(rf, DE_UINT, DE_SIZE_8, channel);
        de_pop_sequence(attr, rf);
        uint8_t* ob = de_push_sequence(attr);
        de_add_number(ob, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_OBEX);
        de_pop_sequence(attr, ob);
    }
    de_pop_sequence(service, attr);
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST);
    attr = de_push_sequence(service);
    de_add_number(attr, DE_UUID, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
    de_pop_sequence(service, attr);
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    attr = de_push_sequence(service);
    {
        uint8_t* p = de_push_sequence(attr);
        de_add_number(p, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
        de_add_number(p, DE_UINT, DE_SIZE_16, 0x0102);   // OPP 1.2
        de_pop_sequence(attr, p);
    }
    de_pop_sequence(service, attr);
    static const char kName[] = "OBEX Object Push";
    de_add_number(service, DE_UINT, DE_SIZE_16, 0x0100);   // service name
    de_add_data(service, DE_STRING, uint16_t(strlen(kName)), (uint8_t*)kName);
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SUPPORTED_FORMATS_LIST);
    attr = de_push_sequence(service);
    de_add_number(attr, DE_UINT, DE_SIZE_8, 0xFF);   // any type of object
    de_pop_sequence(service, attr);
    return de_get_len(service);
}

}  // namespace pd::bt::detail
