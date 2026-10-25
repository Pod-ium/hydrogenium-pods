// pd_usb_drive backend for builds without native USB (host simulator).
#include "msc_backend.h"

namespace pd::usb::detail {

bool available() { return false; }
bool start(hal::BlockDevice*, const DriveOptions&) { return false; }
void task() {}
void stop() {}
Status status() { return Status(); }

}  // namespace pd::usb::detail
