#ifndef BUS_LINKS_H
#define BUS_LINKS_H
#include "board.h"

typedef enum { LINK_USB, LINK_UART1, LINK_UART3, LINK_GAMEPAD } Link;

typedef struct {
    uint16_t size;
    uint8_t bytes[264];
} BusFrame;

void Links_Init(void);
void Links_InputByte(Link link, uint8_t byte);
void Links_InputUsbReport(const uint8_t *data, size_t size);
void Links_ResetUsb(void);
void Links_CheckReceiveTimeouts(uint32_t now);
bool Links_TakeFrame(Link link, BusFrame *frame);
void Links_Send(Link link, const uint8_t *data, size_t size);
void Servo_Send(uint8_t id, uint8_t command, const uint8_t *data, uint8_t size);
void Servo_Set(uint8_t id, uint8_t command, uint16_t value, uint16_t extra);
bool Servo_Read(uint8_t id, uint8_t command, uint8_t *data, uint8_t count);

#endif
