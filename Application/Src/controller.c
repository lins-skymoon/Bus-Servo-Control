#include "actions.h"
#include "controller.h"
#include <string.h>

static uint8_t mode = 1;
static int pad_step = 5;
static uint16_t pad_buttons;
static uint16_t pad_hold;
static uint8_t last_key;
static uint32_t pad_poll;
static uint32_t last_pad;
static uint32_t run_pressed, program_pressed;
static bool run_was_down, run_held, program_was_down, program_held, teaching;
static uint8_t taught_frames;

static void reply(Link source, uint8_t command, const uint8_t *payload, uint8_t size)
{
    uint8_t packet[256] = {0x55, 0x55, (uint8_t)(size + 2u), command};
    if (size) {
        memcpy(packet + 4, payload, size);
    }
    Links_Send(source, packet, size + 4u);
}

static void stop_actions(void)
{
    Actions_Stop();
    reply(LINK_UART1, 7, NULL, 0);
    reply(LINK_UART3, 7, NULL, 0);
}

static void query_list(Link source, uint8_t command, const uint8_t *ids, uint8_t count)
{
    uint8_t result[250] = {0};
    const uint8_t width = command == 0x15 ? 2 : 1;
    const uint8_t query = command == 0x15 ? 0x1C : 0x13;
    uint8_t offset = 1;
    for (unsigned i = 0; i < count && offset + width < sizeof(result); ++i) {
        if (Servo_Read(ids[i], query, result + offset + 1u, width)) {
            result[offset] = ids[i];
            offset += width + 1u;
            ++result[0];
        }
    }
    reply(source, command, result, offset);
}

static void read_configuration(Link source)
{
    static const uint8_t commands[] = {0x15, 0x17, 0x19, 0x22, 0x24, 0x13, 0x1C, 0x1A, 0x1B};
    static const uint8_t widths[] = {4, 4, 1, 1, 1, 1, 2, 1, 2};
    uint8_t data[18];
    if (!Servo_Read(0xFE, 0x0E, data, 1)) {
        return;
    }
    unsigned offset = 1;
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        if (!Servo_Read(data[0], commands[i], data + offset, widths[i])) {
            return;
        }
        offset += widths[i];
    }
    reply(source, 0x1C, data, sizeof(data));
}

static void write_configuration(const uint8_t *data)
{
    Servo_Set(0xFE, 0x0D, data[0], 0);
    Board_Delay(10);
    Servo_Set(0xFE, 0x14, ReadLE16(data + 1), ReadLE16(data + 3));
    Board_Delay(20);
    Servo_Set(0xFE, 0x16, ReadLE16(data + 5), ReadLE16(data + 7));
    Board_Delay(20);
    Servo_Set(0xFE, 0x18, data[9], 0);
    Board_Delay(20);
    Servo_Set(0xFE, 0x21, data[10], 0);
    Board_Delay(30);
    Servo_Set(0xFE, 0x23, data[11], 0);
}

void Controller_Command(Link source, const BusFrame *frame)
{
    if (frame->size < 4 || teaching) {
        return;
    }
    Board_FlashLed();
    const uint8_t command = frame->bytes[3];
    const uint8_t *data = frame->bytes + 4;
    const unsigned size = frame->size - 4u;
    const bool usb = source == LINK_USB;
    uint8_t response[2] = {0, 0};

    switch (command) {
    case 0x01:
        if (usb && size >= 1 && data[0] == 0x4C) {
            response[0] = 0x4F;
            reply(source, command, response, 1);
        }
        break;
    case 0x02:
        if (size >= 5) {
            Servo_Set(data[0], 1, ReadLE16(data + 1), ReadLE16(data + 3));
        }
        break;
    case 0x03:
        if (size >= 3 && data[0] <= (size - 3u) / 3u) {
            const uint16_t duration = ReadLE16(data + 1);
            for (unsigned i = 0; i < data[0]; ++i) {
                const uint8_t *entry = data + 3 + 3u * i;
                Servo_Set(entry[0], 1, ReadLE16(entry + 1), duration);
            }
        }
        break;
    case 0x05:
        if (usb && Actions_Upload(frame)) {
            response[0] = data[0];
            reply(source, command, response, 2);
        }
        break;
    case 0x06:
        if (size >= 3) {
            Actions_Play(data[0], ReadLE16(data + 1));
        }
        break;
    case 0x07:
        stop_actions();
        break;
    case 0x08:
        if (usb && size >= 1 && Actions_Delete(data[0])) {
            reply(source, command, NULL, 0);
        }
        break;
    case 0x0B:
        if (size >= 3) {
            Actions_SetSpeed(data[0], ReadLE16(data + 1));
        }
        break;
    case 0x0F:
        WriteLE16(response, Board_Battery());
        reply(source, command, response, 2);
        break;
    case 0x14: case 0x16:
        if ((usb || command == 0x14) && size && data[0] <= size - 1u) {
            for (unsigned i = 0; i < data[0]; ++i) {
                Servo_Set(data[1u + i], command == 0x14 ? 0x1F : 0x12, 0, 0);
            }
            if (command == 0x16) {
                reply(source, command, NULL, 0);
            }
        }
        break;
    case 0x15: case 0x17:
        if ((usb || command == 0x15) && size && data[0] <= size - 1u) {
            query_list(source, command, data + 1, data[0]);
        }
        break;
    case 0x18:
        if (usb && size >= 2) {
            Servo_Set(data[0], 0x11, data[1], 0);
        }
        break;
    case 0x19:
        if (source == LINK_UART3 && size >= 131u) {
            Actions_WriteRecord(data[0], data[1], data[2], data + 3);
            reply(source, command, NULL, 0);
        }
        break;
    case 0x1A:
        if (usb && size >= 4) {
            Servo_Set(data[0], 0x1D, data[1], ReadLE16(data + 2));
        }
        break;
    case 0x1B:
        if (usb && size >= 12) {
            write_configuration(data);
        }
        break;
    case 0x1C:
        if (usb) {
            read_configuration(source);
        }
        break;
    }
}

static void jog(uint8_t id, int adjustment)
{
    uint8_t data[2];
    if (Servo_Read(id, 2, data, sizeof(data))) {
        int position = (int16_t)ReadLE16(data) + adjustment;
        if (position < 0) {
            position = 0;
        } else if (position > 1000) {
            position = 1000;
        }
        Servo_Set(id, 1, (uint16_t)position, 0);
    }
}

static int axis(uint8_t raw)
{
    const int n = (int8_t)raw;
    return n > 16 ? 16 : (n < -16 ? -16 : n);
}

static void analog_pad(const uint8_t *data)
{
    const uint8_t buttons = data[0];
    int x = axis(data[1]), y = axis(data[2]);
    const int z = (int8_t)data[3];
    if (buttons & 4u) {
        Actions_Play(0, 1);
    }
    if (z != 0) {
        jog(1, (int8_t)(30 * z));
    } else if (x * x > y * y) {
        if (buttons & 3u) {
            jog(2, x);
        } else {
            jog(6, -x / 2);
        }
    } else if (y * y > x * x) {
        if (buttons & 1u) {
            jog(4, -y);
        } else if (buttons & 2u) {
            jog(3, y);
        } else {
            jog(5, y / 2);
        }
    }
}

static uint16_t digital_pad(const uint8_t *data)
{
    uint16_t value = data[0] | (uint16_t)((data[1] & 15u) << 8);
    if (data[2] == 0 || data[4] == 0) {
        value |= 0x1000;
    } else if (data[2] == 4 || data[4] == 255) {
        value |= 0x4000;
    }
    if (data[2] == 6 || data[3] == 0) {
        value |= 0x8000;
    } else if (data[2] == 2 || data[3] == 255) {
        value |= 0x2000;
    }
    if (!(data[0] & 15u)) {
        if (data[6] == 0) {
            value |= 1;
        } else if (data[6] == 255) {
            value |= 4;
        }
        if (data[5] == 0) {
            value |= 8;
        } else if (data[5] == 255) {
            value |= 2;
        }
    }
    return value;
}

static uint8_t pad_key(uint16_t buttons)
{
    static const uint16_t keys[] = {
        0, 0x8000, 0x4000, 0x2000, 0x1000, 0x200, 0x800, 0x400, 0x100,
        8, 4, 2, 1, 0x20, 0x10, 0x80, 0x40
    };
    for (uint8_t i = 1; i <= 16; ++i) {
        if (buttons == keys[i]) {
            return i == 8 ? 0 : i;
        }
        if (buttons == (keys[i] | 0x100u)) {
            return i == 8 ? 0 : i + 16u;
        }
    }
    return 0;
}

static void run_pad_key(uint8_t key, uint8_t event)
{
    static const uint8_t banks[] = {
        0, 3, 2, 4, 1, 0, 0, 0, 0, 7, 6, 8, 5, 10, 9, 12, 11,
        0, 0, 0, 0, 0, 0, 0, 0, 15, 14, 16, 13, 18, 17, 20, 19
    };
    static const int8_t jog_ids[] = {0, 6, 5, -6, -5, 0, 0, 0, 0, -3, -4, 3, 4, 2, -2, -1, 1};
    if (event == 0 && key == 21) {
        mode = mode == 1 ? 2 : 1;
        Board_Beep(mode);
        return;
    }
    if (event == 0 && mode == 2 && (key == 6 || key == 7)) {
        pad_step += key == 6 ? 2 : -2;
        if (pad_step < 1) {
            pad_step = 1;
        } else if (pad_step > 9) {
            pad_step = 9;
        }
        Board_Beep(pad_step == 1 || pad_step == 9 ? 2 : 1);
        return;
    }
    if (key == 5 && (event == 0 || event == 0xA0)) {
        stop_actions();
        Actions_Play(0, 1);
        return;
    }
    const bool release = event == 0x80 || event == 0xC0;
    const bool motion = key < 5 || key == 15 || key == 16;
    if (mode == 1 && banks[key]) {
        if (event == 0 || (event == 0xE0 && key <= 16) || (release && motion)) {
            Actions_Play(banks[key], motion && !release ? 0 : 1);
        }
    } else if (mode == 2 && key <= 16 && jog_ids[key] &&
               (event == 0 || event == 0xE0)) {
        const int id = jog_ids[key];
        jog((uint8_t)(id < 0 ? -id : id), id < 0 ? -pad_step : pad_step);
    }
}

static void service_pad(uint32_t now)
{
    if (now - pad_poll < 25u) {
        return;
    }
    pad_poll = now;
    uint8_t key = pad_key(now - last_pad < 250u ? pad_buttons : 0);
    if (key) {
        if (key != last_key) {
            last_key = key;
            pad_hold = 1;
            run_pad_key(key, 0);
        } else if (pad_hold < 8 && ++pad_hold == 8) {
            run_pad_key(key, 0xA0);
        } else if (pad_hold >= 8) {
            run_pad_key(key, 0xE0);
        }
    } else if (last_key) {
        run_pad_key(last_key, pad_hold < 8 ? 0x80 : 0xC0);
        last_key = 0;
        pad_hold = 0;
    }
}

static void service_buttons(uint32_t now)
{
    const bool run = !Board_PinRead(GPIOB, 5);
    const bool program = !Board_PinRead(GPIOB, 4);
    if (run && !run_was_down) {
        run_pressed = now;
        run_held = false;
    } else if (run && !run_held && now - run_pressed >= 140u) {
        run_held = true;
        if (!teaching) {
            stop_actions();
            Actions_Play(100, 0);
        }
    } else if (!run && run_was_down && !run_held && now - run_pressed >= 6u && !teaching) {
        stop_actions();
        Actions_Play(100, 1);
    }
    if (program && !program_was_down) {
        program_pressed = now;
        program_held = false;
    } else if (program && !program_held && now - program_pressed >= 140u) {
        program_held = true;
        teaching = !teaching;
        taught_frames = 0;
        if (teaching) {
            Actions_Stop();
            for (uint8_t id = 1; id <= 20; ++id) {
                Servo_Set(id, 0x1F, 0, 0);
            }
        }
        Board_Beep(1);
    } else if (!program && program_was_down && !program_held && teaching &&
               now - program_pressed >= 6u && taught_frames < 255u) {
        uint8_t record[128] = {0, 0xE8, 0x03};
        for (uint8_t id = 1; id <= 20; ++id) {
            uint8_t position[2];
            if (Servo_Read(id, 0x1C, position, sizeof(position))) {
                uint8_t *entry = record + 3 + 3u * record[0]++;
                const int value = (int16_t)ReadLE16(position);
                entry[0] = id;
                WriteLE16(entry + 1, (uint16_t)(value < 0 ? 0 : (value > 1000 ? 1000 : value)));
            }
        }
        if (Actions_WriteRecord(100, taught_frames + 1u, taught_frames, record)) {
            ++taught_frames;
            Board_Beep(1);
        }
    }
    run_was_down = run;
    program_was_down = program;
    Board_SetLedIdleLevel(teaching && (now / 100u) % 2u == 0u);
}

void Controller_Service(void)
{
    BusFrame frame;
    for (Link link = LINK_USB; link <= LINK_UART3; ++link) {
        if (Links_TakeFrame(link, &frame)) {
            Controller_Command(link, &frame);
        }
    }
    const uint32_t now = Board_Millis();
    if (Links_TakeFrame(LINK_GAMEPAD, &frame) && !teaching) {
        Board_FlashLed();
        if (frame.size >= 5 && frame.bytes[3] == 0) {
            const uint8_t type = frame.bytes[4];
            if (type == 0 && frame.size >= 12) {
                pad_buttons = digital_pad(frame.bytes + 5);
                last_pad = now;
            } else if ((type == 1 || type == 2) && frame.size >= 9) {
                analog_pad(frame.bytes + 5);
            }
        }
    }
    if (!teaching) {
        service_pad(now);
    }
    service_buttons(now);
}
