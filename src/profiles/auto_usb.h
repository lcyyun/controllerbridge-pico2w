#ifndef DS5_BRIDGE_AUTO_USB_H
#define DS5_BRIDGE_AUTO_USB_H

#include "profile.h"

void auto_usb_init();
BridgeUsbIdentity auto_usb_identity();
const char *auto_usb_identity_name();
void auto_usb_request_identity(BridgeUsbIdentity identity, const char *reason);
void auto_usb_task();

#endif // DS5_BRIDGE_AUTO_USB_H
