#include "links.h"
#include "usb_hid.h"

#define USB(offset) MMIO32(0x40005C00u + (offset))
enum {
    TOP = 0, INT_STATE = 4, EP_INT_STATE = 8, EP0_STATE = 0x0C,
    INT_EN = 0x10, EP_INT_EN = 0x14, EP0_INT_EN = 0x18,
    EP1_STATE = 0x20, EP2_STATE = 0x24, EP1_INT_EN = 0x40, EP2_INT_EN = 0x44,
    ADDRESS = 0x60, EP_EN = 0x64, SETUP = 0x80,
    EP0_AVAIL = 0x100, EP1_AVAIL = 0x104,
    EP0_CTRL = 0x140, EP1_CTRL = 0x144, EP0_FIFO = 0x160, EP1_FIFO = 0x164,
    HALT = 0x188
};

static const uint8_t device[] = {
    0x12, 1, 0, 2, 0, 0, 0, 0x40, 0x83, 4, 0x50, 0x57, 1, 2, 1, 2, 3, 1
};
static const uint8_t configuration[] = {
    9, 2, 41, 0, 1, 1, 0, 0xC0, 0x96, 9, 4, 0, 0, 2, 3, 0, 0, 0,
    9, 0x21, 0x10, 1, 0, 1, 0x22, 33, 0,
    7, 5, 0x81, 3, 0x40, 0, 2, 7, 5, 0x01, 3, 0x40, 0, 2
};
static const uint8_t report[] = {
    5, 0x8C, 9, 1, 0xA1, 1, 9, 3, 0x15, 0, 0x26, 0, 0xFF, 0x75, 8,
    0x95, 64, 0x81, 2, 9, 4, 0x15, 0, 0x26, 0, 0xFF, 0x75, 8, 0x95,
    64, 0x91, 2, 0xC0
};
static const uint8_t language[] = {4, 3, 9, 4};
static const uint8_t maker[60] = {
    60, 3, 'M', 0, 'i', 0, 'n', 0, 'd', 0, 'M', 0, 'o', 0, 't', 0, 'i', 0,
    'o', 0, 'n', 0, ' ', 0, 'S', 0, 'O', 0, 'C', 0, ' ', 0, 'S', 0, 'o', 0,
    'l', 0, 'u', 0, 't', 0, 'i', 0, 'o', 0, 'n', 0, 's', 0
};
/* Keep the original string lengths and padding for host compatibility. */
static const uint8_t product[33] = {33, 3, 'H', 0, 'i', 0, 'w', 0, 'o', 0,
                                   'n', 0, 'd', 0, 'e', 0, 'r', 0};
static const uint8_t serial[32] = {32, 3, 'M', 0, 'M', 0, '3', 0, '2', 0,
                                  'F', 0, '1', 0, '0', 0, '3', 0, 'R', 0, 'B', 0};
static volatile uint8_t configured;
static uint8_t address, idle, protocol;
static bool address_pending, output_report;

static void control_send(const uint8_t *data, uint16_t size, uint16_t requested)
{
    if (size > requested) {
        size = requested;
    }
    for (uint16_t i = 0; i < size; ++i) {
        USB(EP0_FIFO) = data[i];
    }
    USB(EP0_CTRL) = 0x80u | size;
}

static bool descriptor(uint16_t value, uint16_t length)
{
    const uint8_t *data = NULL;
    uint16_t size = 0;
    switch (value) {
    case 0x0100: data = device; size = sizeof(device); break;
    case 0x0200: data = configuration; size = sizeof(configuration); break;
    case 0x0300: data = language; size = sizeof(language); break;
    case 0x0301: data = maker; size = sizeof(maker); break;
    case 0x0302: data = product; size = sizeof(product); break;
    case 0x0303: data = serial; size = sizeof(serial); break;
    case 0x2100: data = configuration + 18; size = 9; break;
    case 0x2200: data = report; size = sizeof(report); break;
    default: return false;
    }
    control_send(data, size, length);
    return true;
}

static void setup_request(void)
{
    uint8_t packet[8];
    for (unsigned i = 0; i < sizeof(packet); ++i) {
        packet[i] = (uint8_t)USB(SETUP + 4u * i);
    }
    const uint8_t type = packet[0];
    const uint8_t request = packet[1];
    const uint16_t value = ReadLE16(packet + 2);
    const uint16_t index = ReadLE16(packet + 4);
    const uint16_t length = ReadLE16(packet + 6);
    uint8_t reply[2] = {0, 0};
    USB(HALT) &= ~BIT(0);
    address_pending = false;
    output_report = false;

    if ((type & 0x60u) == 0) {
        switch (request) {
        case 6:
            if (descriptor(value, length)) {
                return;
            }
            break;
        case 5:
            address = value & 0x7Fu;
            address_pending = true;
            control_send(NULL, 0, 0);
            return;
        case 9:
            configured = (uint8_t)value;
            control_send(NULL, 0, 0);
            return;
        case 8:
            reply[0] = configured;
            control_send(reply, 1, length);
            return;
        case 0:
            if ((type & 31u) == 2u && (index & 15u) == 1u) {
                reply[0] = (USB(HALT) & BIT(1)) != 0;
            } else if ((type & 31u) == 0) {
                reply[0] = 1;
            }
            control_send(reply, 2, length);
            return;
        case 1: case 3:
            if ((type & 31u) == 2u && value == 0 && (index & 15u) == 1u) {
                if (request == 3) {
                    USB(HALT) |= BIT(1);
                } else {
                    USB(HALT) &= ~BIT(1);
                }
                control_send(NULL, 0, 0);
                return;
            }
            break;
        case 10:
            control_send(reply, 1, length);
            return;
        case 11:
            control_send(NULL, 0, 0);
            return;
        }
    } else if ((type & 0x7Fu) == 0x21u) {
        static const uint8_t empty_report[64] = {0};
        switch (request) {
        case 1: control_send(empty_report, sizeof(empty_report), length); return;
        case 2: control_send(&idle, 1, length); return;
        case 3: control_send(&protocol, 1, length); return;
        case 9:
            output_report = length != 0;
            if (!output_report) {
                control_send(NULL, 0, 0);
            }
            return;
        case 10: idle = (uint8_t)(value >> 8); break;
        case 11: protocol = (uint8_t)value; break;
        default: USB(HALT) |= BIT(0); return;
        }
        control_send(NULL, 0, 0);
        return;
    }
    USB(HALT) |= BIT(0);
}

static void reset_endpoints(void)
{
    Links_ResetUsb();
    configured = 0;
    address_pending = false;
    output_report = false;
    protocol = 1;
    idle = 0;
    USB(ADDRESS) = 0;
    USB(HALT) = 0;
    USB(EP_EN) &= ~BIT(0);
    USB(EP0_INT_EN) = 0x45;
    USB(EP0_STATE) = 0x6D;
    USB(EP1_INT_EN) = 4;
    USB(EP1_STATE) = 0x0C;
    USB(EP2_INT_EN) = 0x40;
    USB(EP2_STATE) = 0x60;
    USB(EP_EN) = 7;
    USB(EP_INT_EN) = 7;
    USB(TOP) |= BIT(3);
    USB(TOP) &= ~BIT(3);
}

void Usb_Init(void)
{
    MMIO32(RCC + 0x1C) |= BIT(23);
    USB(TOP) &= ~BIT(1);
    reset_endpoints();
    USB(INT_STATE) = 0x11;
    USB(INT_EN) = 0x11;
    Board_EnableIrq(IRQ_USB, 0x80);
    USB(TOP) |= BIT(1);
}

static __attribute__((noinline)) void receive_report(unsigned available, unsigned fifo)
{
    uint8_t data[64];
    const unsigned count = USB(available) & 0xFFu;
    if (count > sizeof(data)) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        data[i] = fifo == EP1_FIFO ? MMIO8(0x40005C00u + EP1_FIFO) : (uint8_t)USB(fifo);
    }
    Links_InputUsbReport(data, count);
}

void USB_IRQHandler(void)
{
    const uint32_t flags = USB(INT_STATE);
    if (flags & 1u) {
        USB(INT_STATE) = 1;
        reset_endpoints();
        return;
    }
    if (!(flags & BIT(4))) {
        return;
    }
    USB(INT_STATE) = BIT(4);
    const uint32_t endpoints = USB(EP_INT_STATE);
    if (endpoints & BIT(0)) {
        USB(EP_INT_STATE) = BIT(0);
        const uint32_t ep = USB(EP0_STATE);
        if (ep & BIT(0)) {
            USB(EP0_STATE) = BIT(0);
            setup_request();
        } else if (ep & BIT(6)) {
            USB(EP0_STATE) = 0x60;
            if (address_pending) {
                USB(ADDRESS) = address;
                address_pending = false;
            }
        } else if (ep & (BIT(2) | BIT(3))) {
            USB(EP0_STATE) = 0x0C;
            if (output_report) {
                receive_report(EP0_AVAIL, EP0_FIFO);
                output_report = false;
                control_send(NULL, 0, 0);
            }
        }
    }
    if (endpoints & BIT(1)) {
        USB(EP_INT_STATE) = BIT(1);
        const uint32_t ep = USB(EP1_STATE);
        if (ep & (BIT(2) | BIT(3))) {
            USB(EP1_STATE) = 0x0C;
            receive_report(EP1_AVAIL, EP1_FIFO);
        }
        if (ep & BIT(6)) {
            USB(EP1_STATE) = 0x60;
        }
    }
    if (endpoints & BIT(2)) {
        USB(EP_INT_STATE) = BIT(2);
        USB(EP2_STATE) = 0x60;
    }
}

void Usb_Write(const uint8_t *data, size_t size)
{
    while (configured && size) {
        const uint32_t start = Board_Millis();
        while (USB(EP1_CTRL) & BIT(7)) {
            if (Board_Millis() - start >= 120u || !configured) {
                return;
            }
        }
        const size_t count = size > 64u ? 64u : size;
        for (size_t i = 0; i < count; ++i) {
            USB(EP1_FIFO) = data[i];
        }
        for (size_t i = count; i < 64u; ++i) {
            USB(EP1_FIFO) = 0;
        }
        USB(EP1_CTRL) = BIT(7) | 64u;
        data += count;
        size -= count;
    }
}
