#ifndef BUS_BOARD_H
#define BUS_BOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MMIO32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MMIO16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define MMIO8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define BIT(n) (1u << (n))
#define GPIOA 0x40010800u
#define GPIOB 0x40010C00u
#define RCC 0x40021000u
#define UART1 0x40013800u
#define UART2 0x40004400u
#define UART3 0x40004800u
#define SPI1 0x40013000u
#define SPI2 0x40003800u

enum { IRQ_USB = 19, IRQ_EXTI5 = 23, IRQ_SPI2 = 36,
       IRQ_UART1 = 37, IRQ_UART2 = 38, IRQ_UART3 = 39 };

void Board_Init(void);
void Board_EnableIrq(unsigned irq, uint8_t priority);
void Board_DisableIrq(unsigned irq);
void Board_PinMode(uint32_t port, unsigned pin, uint32_t mode);
void Board_PinWrite(uint32_t port, unsigned pin, bool high);
bool Board_PinRead(uint32_t port, unsigned pin);
uint32_t Board_Millis(void);
void Board_Delay(uint32_t ms);
void Board_CheckStartupVoltage(void);
_Noreturn void Board_LatchAlarm(void);
uint16_t Board_Battery(void);
void Board_Beep(unsigned pattern);
void Board_SerialError(void);
void Board_FlashLed(void);
void Board_SetLedIdleLevel(bool high);
void Board_Service(void);

static inline uint16_t ReadLE16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static inline void WriteLE16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}
#endif
