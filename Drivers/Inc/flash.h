#ifndef BUS_FLASH_H
#define BUS_FLASH_H
#include "board.h"
bool Flash_Init(void);
bool Flash_Read(uint32_t address, void *buffer, size_t size);
bool Flash_Write(uint32_t address, const void *buffer, size_t size);
bool Flash_EraseSector(uint32_t address);
#endif
