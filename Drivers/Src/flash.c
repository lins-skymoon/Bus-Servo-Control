#include "flash.h"
#include <string.h>

enum { PAGE_SIZE = 256, SECTOR_SIZE = 4096, FLASH_CAPACITY = 0x1000000 };
static uint8_t sector[SECTOR_SIZE];
static bool flash_present;

static uint8_t transfer(uint8_t byte)
{
    MMIO32(SPI1) = byte;
    while (!(MMIO16(SPI1 + 8) & BIT(0))) {
    }
    while (!(MMIO16(SPI1 + 8) & BIT(1))) {
    }
    return (uint8_t)MMIO32(SPI1 + 4);
}

static void select(bool active)
{
    Board_PinWrite(GPIOA, 4, !active);
}

static void address_command(uint8_t command, uint32_t address)
{
    select(true);
    transfer(command);
    transfer((uint8_t)(address >> 16));
    transfer((uint8_t)(address >> 8));
    transfer((uint8_t)address);
}

bool Flash_Init(void)
{
    MMIO32(RCC + 0x18) |= BIT(12);
    Board_PinWrite(GPIOA, 4, true);
    Board_PinMode(GPIOA, 4, 3);
    Board_PinMode(GPIOA, 5, 0xB);
    Board_PinMode(GPIOA, 6, 8);
    Board_PinWrite(GPIOA, 6, true);
    Board_PinMode(GPIOA, 7, 0xB);
    MMIO16(SPI1 + 0x1C) = 0x19;
    MMIO16(SPI1 + 0x20) = 2;
    MMIO16(SPI1 + 0x18) = 0x1D;
    address_command(0x90, 0);
    transfer(0xFF);
    flash_present = transfer(0xFF) == 0x17;
    select(false);
    if (flash_present) {
        select(true);
        transfer(0x04);
        select(false);
    }
    return flash_present;
}

bool Flash_Read(uint32_t address, void *buffer, size_t size)
{
    if (!flash_present || address >= FLASH_CAPACITY || size > FLASH_CAPACITY - address) {
        return false;
    }
    uint8_t *bytes = buffer;
    address_command(0x03, address);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = transfer(0xFF);
    }
    select(false);
    return true;
}

static void write_enable(void)
{
    select(true);
    transfer(0x06);
    select(false);
}

static bool wait_ready(uint32_t timeout)
{
    uint32_t start = Board_Millis();
    select(true);
    transfer(0x05);
    while (transfer(0xFF) & 1u) {
        if (Board_Millis() - start >= timeout) {
            select(false);
            return false;
        }
    }
    select(false);
    return true;
}

bool Flash_EraseSector(uint32_t address)
{
    if (!flash_present || address >= FLASH_CAPACITY) {
        return false;
    }
    write_enable();
    address_command(0x20, address & ~(SECTOR_SIZE - 1u));
    select(false);
    return wait_ready(800);
}

static bool program(uint32_t address, const uint8_t *data, size_t size)
{
    while (size) {
        size_t n = PAGE_SIZE - (address % PAGE_SIZE);
        if (n > size) {
            n = size;
        }
        write_enable();
        address_command(0x02, address);
        for (size_t i = 0; i < n; ++i) {
            transfer(data[i]);
        }
        select(false);
        if (!wait_ready(20)) {
            return false;
        }
        data += n;
        address += (uint32_t)n;
        size -= n;
    }
    return true;
}

bool Flash_Write(uint32_t address, const void *buffer, size_t size)
{
    if (!flash_present || address >= FLASH_CAPACITY || size > FLASH_CAPACITY - address) {
        return false;
    }
    const uint8_t *data = buffer;
    while (size > 0) {
        const uint32_t sector_base = address & ~(SECTOR_SIZE - 1u);
        const uint32_t offset = address - sector_base;
        size_t n = SECTOR_SIZE - offset;
        if (n > size) {
            n = size;
        }
        if (!Flash_Read(sector_base, sector, sizeof(sector))) {
            return false;
        }
        bool erase = false;
        for (size_t i = 0; i < n; ++i) {
            if ((sector[offset + i] & data[i]) != data[i]) {
                erase = true;
                break;
            }
        }
        if (erase) {
            memcpy(sector + offset, data, n);
            if (!Flash_EraseSector(sector_base) || !program(sector_base, sector, sizeof(sector))) {
                return false;
            }
        } else if (!program(address, data, n)) {
            return false;
        }
        address += (uint32_t)n;
        data += n;
        size -= n;
    }
    return true;
}
