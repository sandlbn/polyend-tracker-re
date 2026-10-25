/* Host build of the USB descriptors (third_party/teensy3/usb_desc.c, compiled
 * with OT_USB_DESC_HOST_TEST) for tests/usb_desc_test.cpp: exposes the static
 * descriptor arrays. Built separately with OT_USB_MSC=1 and OT_USB_MSC=0. */
#include "usb_desc.c"

const uint8_t *ot_usb_device_desc(unsigned *n) { *n = sizeof(device_descriptor); return device_descriptor; }
const uint8_t *ot_usb_config_desc(unsigned *n) { *n = sizeof(config_descriptor); return config_descriptor; }
const uint8_t *ot_usb_ep_config(unsigned *n) { *n = NUM_ENDPOINTS; return usb_endpoint_config_table; }
