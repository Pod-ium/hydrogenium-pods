// BTstack configuration for the host lifecycle test (real BTstack core, dummy
// radio): Classic, heap allocation, as on the device.
#pragma once
#define ENABLE_CLASSIC
#define HAVE_MALLOC
#define HAVE_ASSERT
#define HAVE_POSIX_TIME
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define NVM_NUM_LINK_KEYS 16
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 16
