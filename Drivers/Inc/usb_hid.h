#ifndef BUS_USB_HID_H
#define BUS_USB_HID_H
#include "board.h"
void Usb_Init(void);
void Usb_Write(const uint8_t *data, size_t size);
#endif
