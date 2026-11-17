#include "actions.h"
#include "flash.h"
#include <string.h>

enum {
    MARKER = 0x1000, BANK_FLAGS = 0x1200, BUTTON_SETTINGS = 0x1300, TRACK_LENGTHS = 0x2000,
    SEQUENCE_COUNT = 0x2100, SEQUENCE_DATA = 0x2102, RECORDS = 0x3000,
    RECORD_SIZE = 128, SEQUENCE_LIMIT = 1024, FLASH_SIZE = 0x1000000
};
static uint8_t valid[256], tracks[256], upload_tracks[256], sequence[SEQUENCE_LIMIT];
static uint16_t speed[256];
static uint16_t sequence_size, sequence_index, frame_index, remaining, requested_repeats;
static uint8_t running_bank, upload_bank;
static bool running, upload_ready;
static uint32_t due;

static void notify_serial(uint8_t command, uint8_t bank, uint16_t repeats)
{
    uint8_t packet[] = {0x55, 0x55, 5, command, bank, 0, 0};
    WriteLE16(packet + 5, repeats);
    Links_Send(LINK_UART3, packet, sizeof(packet));
    Links_Send(LINK_UART1, packet, sizeof(packet));
}

static uint32_t bank_address(uint8_t bank, uint32_t offset)
{
    return ((uint32_t)bank << 16) + offset;
}

static uint32_t record_address(uint8_t bank, const uint8_t *lengths, uint8_t track, uint16_t frame)
{
    uint32_t ordinal = frame;
    for (unsigned i = 0; i < track; ++i) {
        ordinal += lengths[i];
    }
    return bank_address(bank, RECORDS + ordinal * RECORD_SIZE);
}

void Actions_Init(void)
{
    char marker[5];
    for (unsigned i = 0; i < 256; ++i) {
        speed[i] = 100;
    }
    if (!Flash_Read(MARKER, marker, sizeof(marker))) {
        return;
    }
    if (memcmp(marker, "LOBOT", 5) == 0) {
        Flash_Read(BANK_FLAGS, valid, sizeof(valid));
    } else {
        const uint8_t settings[33] = {0};
        if (Flash_Write(BANK_FLAGS, valid, sizeof(valid)) &&
            Flash_Write(BUTTON_SETTINGS, settings, sizeof(settings))) {
            Flash_Write(MARKER, "LOBOT", 5);
        }
    }
}

static bool set_valid(uint8_t bank, bool state)
{
    const uint8_t flag = state ? 1 : 0;
    if (!Flash_Write(BANK_FLAGS + bank, &flag, 1)) {
        return false;
    }
    valid[bank] = flag;
    return true;
}

bool Actions_Play(uint8_t bank, uint16_t repeats)
{
    if (valid[bank] != 1) {
        return false;
    }
    if (running && running_bank == bank) {
        requested_repeats = remaining = repeats;
        return true;
    }
    if (running && bank != 0) {
        return false;
    }
    uint8_t count[2];
    running = false;
    if (!Flash_Read(bank_address(bank, TRACK_LENGTHS), tracks, sizeof(tracks)) ||
        !Flash_Read(bank_address(bank, SEQUENCE_COUNT), count, 2)) {
        return false;
    }
    sequence_size = ReadLE16(count);
    if (!sequence_size || sequence_size > sizeof(sequence) ||
        !Flash_Read(bank_address(bank, SEQUENCE_DATA), sequence, sequence_size)) {
        return false;
    }
    sequence_index = frame_index = 0;
    requested_repeats = remaining = repeats;
    running_bank = bank;
    due = Board_Millis();
    running = true;
    notify_serial(6, bank, repeats);
    return true;
}

void Actions_Stop(void)
{
    running = false;
}

void Actions_SetSpeed(uint8_t bank, uint16_t percent)
{
    if (percent == 0) {
        return;
    }
    if (bank == 255) {
        for (unsigned i = 0; i < 256; ++i) {
            speed[i] = percent;
        }
    } else {
        speed[bank] = percent;
    }
}

void Actions_Service(void)
{
    if (!running || (int32_t)(Board_Millis() - due) < 0) {
        return;
    }
    while (sequence_index < sequence_size && frame_index >= tracks[sequence[sequence_index]]) {
        ++sequence_index;
        frame_index = 0;
    }
    if (sequence_index == sequence_size) {
        if (remaining == 1) {
            Actions_Stop();
            notify_serial(8, running_bank, requested_repeats);
            return;
        }
        if (remaining > 1) {
            --remaining;
        }
        sequence_index = 0;
        while (sequence_index < sequence_size && tracks[sequence[sequence_index]] == 0) {
            ++sequence_index;
        }
        if (sequence_index == sequence_size) {
            Actions_Stop();
            return;
        }
    }
    const uint32_t address = record_address(running_bank, tracks, sequence[sequence_index], frame_index++);
    uint8_t record[RECORD_SIZE];
    if (address > FLASH_SIZE - RECORD_SIZE || !Flash_Read(address, record, sizeof(record)) ||
        record[0] > (RECORD_SIZE - 3u) / 3u) {
        Actions_Stop();
        return;
    }
    uint32_t duration = (uint32_t)ReadLE16(record + 1) * 100u / speed[running_bank];
    if (duration < 50u) {
        duration = 50;
    } else if (duration > 30000u) {
        duration = 30000;
    }
    for (unsigned i = 0; i < record[0]; ++i) {
        const uint8_t *entry = record + 3 + 3u * i;
        Servo_Set(entry[0], 1, ReadLE16(entry + 1), (uint16_t)duration);
    }
    due = Board_Millis() + duration;
}

bool Actions_Delete(uint8_t bank)
{
    if (running && (bank == 255 || bank == running_bank)) {
        Actions_Stop();
    }
    Actions_SetSpeed(bank, 100);
    if (bank == 255) {
        uint8_t cleared[256] = {0};
        if (!Flash_Write(BANK_FLAGS, cleared, sizeof(cleared))) {
            return false;
        }
        memset(valid, 0, sizeof(valid));
        return true;
    }
    return set_valid(bank, false);
}

static bool ensure_marker(void)
{
    char marker[5];
    if (!Flash_Read(MARKER, marker, sizeof(marker))) {
        return false;
    }
    return memcmp(marker, "LOBOT", 5) == 0 || Flash_Write(MARKER, "LOBOT", 5);
}

bool Actions_WriteRecord(uint8_t bank, uint8_t count, uint8_t index, const uint8_t *data)
{
    if (index >= count || !ensure_marker()) {
        return false;
    }
    const uint32_t address = bank_address(bank, RECORDS + (uint32_t)index * RECORD_SIZE);
    if (address > FLASH_SIZE - RECORD_SIZE ||
        (!(index % 32u) && !Flash_EraseSector(address)) ||
        !Flash_Write(address, data, RECORD_SIZE)) {
        return false;
    }
    if (index + 1u == count) {
        const uint8_t single_track[] = {1, 0, 0};
        speed[bank] = 100;
        return Flash_EraseSector(bank_address(bank, TRACK_LENGTHS)) &&
               Flash_Write(bank_address(bank, TRACK_LENGTHS), &count, 1) &&
               Flash_Write(bank_address(bank, SEQUENCE_COUNT), single_track, sizeof(single_track)) &&
               set_valid(bank, true);
    }
    return true;
}

bool Actions_Upload(const BusFrame *frame)
{
    if (frame->size < 6u) {
        return false;
    }
    const uint8_t *data = frame->bytes;
    const uint8_t type = data[4], bank = data[5];
    const size_t payload = frame->size - 6u;
    if (type == 1 && payload && payload <= sizeof(upload_tracks)) {
        upload_ready = false;
        memset(upload_tracks, 0, sizeof(upload_tracks));
        memcpy(upload_tracks, data + 6, payload);
        unsigned records = 0;
        for (size_t i = 0; i < payload; ++i) {
            records += upload_tracks[i];
        }
        const uint32_t limit = bank == 255u ? FLASH_SIZE : bank_address(bank + 1u, TRACK_LENGTHS);
        if (records > (limit - bank_address(bank, RECORDS)) / RECORD_SIZE) {
            return false;
        }
        if (running && running_bank == bank) {
            Actions_Stop();
        }
        upload_ready = ensure_marker() && set_valid(bank, false) &&
                       Flash_EraseSector(bank_address(bank, TRACK_LENGTHS)) &&
                       Flash_Write(bank_address(bank, TRACK_LENGTHS), upload_tracks, sizeof(upload_tracks));
        upload_bank = bank;
        return upload_ready;
    }
    if (!upload_ready || bank != upload_bank) {
        return false;
    }
    if (type == 2 && payload == 2u) {
        const uint16_t count = ReadLE16(data + 6);
        return count && count <= SEQUENCE_LIMIT &&
               Flash_Write(bank_address(bank, SEQUENCE_COUNT), data + 6, 2);
    }
    if (type == 3 && payload >= 2u && payload <= 201u && data[6] <= 18u) {
        uint8_t sequence_block[200] = {0};
        memcpy(sequence_block, data + 7, payload - 1u);
        return Flash_Write(bank_address(bank, SEQUENCE_DATA + 200u * data[6]),
                           sequence_block, sizeof(sequence_block));
    }
    if (type == 4 && payload >= 5u && payload <= 2u + RECORD_SIZE &&
        data[7] < upload_tracks[data[6]]) {
        const size_t record_length = 3u + 3u * data[8];
        if (record_length > RECORD_SIZE ||
            (payload - 2u != record_length && payload - 2u != RECORD_SIZE)) {
            return false;
        }
        const uint32_t address = record_address(bank, upload_tracks, data[6], data[7]);
        uint8_t record[RECORD_SIZE] = {0};
        memcpy(record, data + 8, record_length);
        return ((address & 4095u) || Flash_EraseSector(address)) &&
               Flash_Write(address, record, sizeof(record));
    }
    if (type == 5 && !payload && set_valid(bank, true)) {
        upload_ready = false;
        speed[bank] = 100;
        Board_Beep(1);
        return true;
    }
    return false;
}
