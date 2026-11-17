#ifndef BUS_ACTIONS_H
#define BUS_ACTIONS_H
#include "links.h"

void Actions_Init(void);
bool Actions_Play(uint8_t bank, uint16_t repeats);
void Actions_Stop(void);
void Actions_SetSpeed(uint8_t bank, uint16_t percent);
void Actions_Service(void);
bool Actions_Delete(uint8_t bank);
bool Actions_Upload(const BusFrame *frame);
bool Actions_WriteRecord(uint8_t bank, uint8_t count, uint8_t index, const uint8_t *data);

#endif
