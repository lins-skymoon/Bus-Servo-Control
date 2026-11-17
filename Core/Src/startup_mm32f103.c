#include "board.h"

extern const uint8_t Image$$ARM_LIB_STACK$$ZI$$Limit;
extern void __main(void) __attribute__((noreturn));

void Reset_Handler(void);
void SysTick_Handler(void);
void USB_IRQHandler(void);
void TIM2_IRQHandler(void);
void SPI2_IRQHandler(void);
void UART1_IRQHandler(void);
void UART2_IRQHandler(void);
void UART3_IRQHandler(void);

static void Default_Handler(void)
{
    for (;;) {
    }
}

/* MM32F103 uses IRQ19 for USB and IRQ36 for SPI2. */
typedef union {
    const void *stack;
    void (*handler)(void);
} Vector;
#define ISR(fn) { .handler = fn }
#define D ISR(Default_Handler)

__attribute__((section("RESET"), used, aligned(256)))
const Vector vectors[59] = {
    { .stack = &Image$$ARM_LIB_STACK$$ZI$$Limit }, ISR(Reset_Handler),
    D, D, D, D, D, {0}, {0}, {0}, {0}, D, D, {0}, D, ISR(SysTick_Handler),
    D, D, D, D, D, D, D, D, D, D, D, D, D, D, D, D, D, D, D,
    ISR(USB_IRQHandler), D, D, D, D, D, D, D, D, ISR(TIM2_IRQHandler), D, D, D, D, D, D, D,
    ISR(SPI2_IRQHandler), ISR(UART1_IRQHandler), ISR(UART2_IRQHandler),
    ISR(UART3_IRQHandler), D, D, D
};

void Reset_Handler(void)
{
    __main();
}
