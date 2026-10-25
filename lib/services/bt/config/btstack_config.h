// BTstack configuration for pd_bt on the Pico W / Pico 2 W (CYW43):
// Bluetooth Classic only (OBEX Object Push over RFCOMM), memory from malloc.
#pragma once

#ifndef ENABLE_CLASSIC
#define ENABLE_CLASSIC
#endif
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO   // captured by pd_bt (Diagnostics), not printed
#define ENABLE_PRINTF_HEXDUMP
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HAVE_MALLOC   // BTstack objects from the heap: no per-type pool sizes to tune

// HCI / ACL buffers (CYW43 transport needs the pre-buffer and alignment)
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// At most 3 ACL / SCO packets queued in the CYW43 at once (BTstack would
// otherwise trust the controller's reported count).
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// Pairing keys (kept by pd_bt in a file on the SD card)
#define NVM_NUM_LINK_KEYS 16
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 16
#define MAX_ATT_DB_SIZE 512
