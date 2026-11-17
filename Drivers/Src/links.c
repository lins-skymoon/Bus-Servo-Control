#include "links.h"
#include "usb_hid.h"
#include <string.h>

enum { UART_TDR = 0, UART_RDR = 4, UART_STATUS = 8, UART_ISR = 0xC,
       UART_IER = 0x10, UART_ICR = 0x14, UART_GCR = 0x18, UART_CCR = 0x1C };

enum { UART3_RECORD_PACKET_SIZE = 135, UART1_TIMEOUT_MS = 180, UART3_TIMEOUT_MS = 100 };

typedef struct {
    BusFrame work;
    BusFrame ready;
    volatile bool pending;
    uint32_t started;
    uint16_t expected;
} Receiver;

static Receiver receivers[4];
static volatile uint8_t servo_bytes[32];
static volatile unsigned servo_used;
static volatile uint8_t servo_size;
static volatile bool servo_done;

static void queue_frame(Receiver *rx)
{
    if (!rx->pending) {
        rx->ready = rx->work;
        rx->pending = true;
    }
    rx->work.size = 0;
    rx->expected = 0;
}

static void uart_init(uint32_t base, uint32_t divisor)
{
    MMIO32(base + UART_CCR) = 0x30;
    MMIO32(base + 0x20) = divisor >> 4;
    MMIO32(base + 0x24) = divisor & 15;
    MMIO32(base + UART_GCR) = 0x19;
    MMIO32(base + UART_IER) = 2;
    MMIO32(base + UART_ICR) = 2;
}

void Links_Init(void)
{
    MMIO32(RCC + 0x18) |= BIT(14);
    MMIO32(RCC + 0x1C) |= BIT(14) | BIT(17) | BIT(18);
    Board_PinMode(GPIOA, 0, 3);
    Board_PinMode(GPIOA, 1, 3);
    Board_PinWrite(GPIOA, 1, false);
    Board_PinWrite(GPIOA, 0, true);
    Board_PinMode(GPIOA, 2, 0xB);
    Board_PinMode(GPIOA, 3, 8);
    Board_PinWrite(GPIOA, 3, true);
    Board_PinMode(GPIOA, 9, 0xB);
    Board_PinMode(GPIOA, 10, 8);
    Board_PinWrite(GPIOA, 10, true);
    Board_PinMode(GPIOB, 10, 0xB);
    Board_PinMode(GPIOB, 11, 8);
    Board_PinWrite(GPIOB, 11, true);
    uart_init(UART1, 5000);
    uart_init(UART2, 208);
    uart_init(UART3, 2500);
    Board_EnableIrq(IRQ_UART1, 0x40);
    Board_EnableIrq(IRQ_UART2, 0x50);
    Board_EnableIrq(IRQ_UART3, 0x60);

    Board_PinMode(GPIOB, 12, 4);
    Board_PinMode(GPIOB, 13, 4);
    Board_PinMode(GPIOB, 14, 0xB);
    Board_PinMode(GPIOB, 15, 4);
    MMIO16(SPI2 + 0x1C) = 0x19;
    MMIO16(SPI2 + 0x20) = 0x10;
    MMIO16(SPI2 + 0x10) = 2;
    MMIO16(SPI2 + 0x14) = 2;
    MMIO16(SPI2 + 0x18) = 0x1B;
    Board_EnableIrq(IRQ_SPI2, 0xA0);
}

void Links_InputByte(Link link, uint8_t byte)
{
    static const uint16_t limit[] = {264, 110, 30, 20};
    Receiver *rx = &receivers[link];
    BusFrame *frame = &rx->work;
    const uint32_t now = Board_Millis();
    const bool host_uart = link == LINK_UART1 || link == LINK_UART3;
    if (!host_uart && frame->size && now - rx->started > 180u) {
        frame->size = 0;
    }
    if (frame->size < 2) {
        if (byte != 0x55) {
            frame->size = 0;
            if (host_uart) {
                Board_SerialError();
            }
            return;
        }
        if (!frame->size) {
            rx->expected = 0;
        }
        if (!host_uart || !frame->size) {
            rx->started = now;
        }
    } else if (frame->size == 2) {
        rx->expected = (uint16_t)byte + 2;
        const bool long_record = link == LINK_UART3 && rx->expected == UART3_RECORD_PACKET_SIZE;
        if (byte < 2 || (rx->expected > limit[link] && !long_record)) {
            frame->size = 0;
            if (host_uart) {
                Board_SerialError();
            }
            return;
        }
    }
    frame->bytes[frame->size++] = byte;
    if (link == LINK_UART3 && frame->size == 4) {
        if (byte == 0x19) {
            rx->expected = UART3_RECORD_PACKET_SIZE;
        } else if (rx->expected > limit[link]) {
            frame->size = 0;
            Board_SerialError();
            return;
        }
    }
    if (frame->size >= 4 && frame->size == rx->expected) {
        queue_frame(rx);
    }
}

void Links_InputUsbReport(const uint8_t *data, size_t size)
{
    Receiver *rx = &receivers[LINK_USB];
    BusFrame *frame = &rx->work;
    const uint32_t now = Board_Millis();
    if (frame->size && now - rx->started >= 800u) {
        frame->size = 0;
    }
    if (!frame->size) {
        if (size < 4u || data[0] != 0x55 || data[1] != 0x55 || data[2] < 2u) {
            return;
        }
        rx->expected = (uint16_t)data[2] + 2u;
    }
    rx->started = now;
    /* Windows removes its leading report ID. The endpoint carries 64 payload bytes. */
    const size_t remaining = rx->expected - frame->size;
    if (size > remaining) {
        size = remaining;
    }
    memcpy(frame->bytes + frame->size, data, size);
    frame->size += (uint16_t)size;
    if (frame->size == rx->expected) {
        queue_frame(rx);
    }
}

void Links_ResetUsb(void)
{
    receivers[LINK_USB].work.size = 0;
    receivers[LINK_USB].expected = 0;
    receivers[LINK_USB].pending = false;
}

void Links_CheckReceiveTimeouts(uint32_t now)
{
    uint32_t primask;
    bool expired = false;
    /* UART IRQs can preempt SysTick. Protect the check and reset together. */
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
    for (Link link = LINK_UART1; link <= LINK_UART3; ++link) {
        Receiver *rx = &receivers[link];
        const uint32_t timeout = link == LINK_UART3 && rx->expected != UART3_RECORD_PACKET_SIZE
                               ? UART3_TIMEOUT_MS : UART1_TIMEOUT_MS;
        if (rx->work.size && now - rx->started >= timeout) {
            rx->work.size = 0;
            rx->expected = 0;
            expired = true;
        }
    }
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
    if (expired) {
        Board_SerialError();
    }
}

bool Links_TakeFrame(Link link, BusFrame *frame)
{
    Receiver *rx = &receivers[link];
    if (!rx->pending) {
        return false;
    }
    *frame = rx->ready;
    rx->pending = false;
    return true;
}

static void uart_write(uint32_t uart, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        while (!(MMIO32(uart + UART_STATUS) & BIT(3))) {
        }
        MMIO32(uart + UART_TDR) = data[i];
    }
    while (!(MMIO32(uart + UART_STATUS) & BIT(0))) {
    }
}

void Links_Send(Link link, const uint8_t *data, size_t size)
{
    if (link == LINK_USB) {
        Usb_Write(data, size);
    } else if (link == LINK_UART1 || link == LINK_UART3) {
        uart_write(link == LINK_UART1 ? UART1 : UART3, data, size);
    }
}

static uint8_t uart_read(uint32_t uart)
{
    MMIO32(uart + UART_ICR) = 2;
    return (uint8_t)MMIO32(uart + UART_RDR);
}

void UART1_IRQHandler(void)
{
    if (MMIO32(UART1 + UART_ISR) & 2u) {
        Links_InputByte(LINK_UART1, uart_read(UART1));
    }
}

void UART3_IRQHandler(void)
{
    if (MMIO32(UART3 + UART_ISR) & 2u) {
        Links_InputByte(LINK_UART3, uart_read(UART3));
    }
}

void SPI2_IRQHandler(void)
{
    if (MMIO16(SPI2 + 0x0C) & 2u) {
        MMIO16(SPI2 + 0x14) = 2u;
        Links_InputByte(LINK_GAMEPAD, (uint8_t)MMIO32(SPI2 + 4));
    }
}

void UART2_IRQHandler(void)
{
    if (!(MMIO32(UART2 + UART_ISR) & 2u)) {
        return;
    }
    uint8_t byte = uart_read(UART2);
    if (servo_done) {
        return;
    }
    if (servo_used < 2 && byte != 0x55) {
        servo_used = 0;
        return;
    }
    servo_bytes[servo_used++] = byte;
    if (servo_used == 4) {
        servo_size = byte + 3u;
        if (byte < 3u || byte > sizeof(servo_bytes) - 3u) {
            servo_used = 0;
        }
    }
    if (servo_used >= 6 && servo_used == servo_size) {
        servo_done = true;
    }
}

void Servo_Send(uint8_t id, uint8_t command, const uint8_t *data, uint8_t size)
{
    uint8_t frame[16] = {0x55, 0x55, id, (uint8_t)(size + 3u), command};
    if (size > sizeof(frame) - 6u) {
        return;
    }
    uint8_t sum = id + frame[3] + command;
    for (uint8_t i = 0; i < size; ++i) {
        frame[5u + i] = data[i];
        sum += data[i];
    }
    frame[5u + size] = (uint8_t)~sum;
    Board_PinWrite(GPIOA, 1, true);
    Board_PinWrite(GPIOA, 0, false);
    uart_write(UART2, frame, 6u + size);
    Board_PinWrite(GPIOA, 1, false);
    Board_PinWrite(GPIOA, 0, true);
}

void Servo_Set(uint8_t id, uint8_t command, uint16_t value, uint16_t extra)
{
    uint8_t bytes[4];
    WriteLE16(bytes, value);
    WriteLE16(bytes + 2, extra);
    uint8_t size = 1;
    switch (command) {
    case 0x0C: case 0x12:
        size = 0;
        break;
    case 1: case 0x14: case 0x16: case 0x1D:
        size = 4;
        break;
    }
    Servo_Send(id, command, bytes, size);
}

bool Servo_Read(uint8_t id, uint8_t command, uint8_t *data, uint8_t count)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        servo_used = 0;
        servo_done = false;
        Servo_Send(id, command, NULL, 0);
        const uint32_t start = Board_Millis();
        while (!servo_done && Board_Millis() - start < 20u) {
        }
        if (!servo_done || servo_size < 6u + count || servo_bytes[4] != command ||
            (id != 0xFE && servo_bytes[2] != id)) {
            continue;
        }
        uint8_t sum = 0;
        for (unsigned i = 2; i + 1 < servo_size; ++i) {
            sum += servo_bytes[i];
        }
        if ((uint8_t)~sum != servo_bytes[servo_size - 1u]) {
            continue;
        }
        for (uint8_t i = 0; i < count; ++i) {
            data[i] = servo_bytes[5u + i];
        }
        return true;
    }
    return false;
}
