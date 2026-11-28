#pragma once
// usb 3 (xhci) host controllers, with usb keyboards and mice on them
#include <stdbool.h>

// takes the controllers over from the firmware and starts the usb thread.
// true if there was at least one
bool xhci_init(void);
