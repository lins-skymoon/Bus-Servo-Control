#include "board.h"
#include "links.h"

#define ADC1 0x40012400u
#define ADC2 0x40012800u
#define TIM2 0x40000000u

enum {
    BATTERY_CHECK_INTERVAL_MS = 500,
    LOW_BATTERY_MV = 5600,
    CRITICAL_BATTERY_MV = 4400,
    LOW_BATTERY_SAMPLES = 11,
    CRITICAL_BATTERY_SAMPLES = 2
};

static volatile uint32_t milliseconds;
static uint32_t next_battery_check;
static uint16_t battery_mv;
static unsigned low_battery_samples, critical_battery_samples;
static volatile uint32_t beep_age;
static volatile unsigned beep_pattern;
static volatile bool serial_error_pending;
static volatile bool alarm_latched;
static volatile uint32_t alarm_latched_at;
static volatile bool led_idle_high, led_flash_requested;
static unsigned led_flash_ticks;
static bool buzzer_driven;

void Board_EnableIrq(unsigned irq, uint8_t priority)
{
    MMIO8(0xE000E400u + irq) = priority;
    MMIO32(0xE000E100u + (irq / 32u) * 4u) = BIT(irq % 32u);
}

void Board_DisableIrq(unsigned irq)
{
    MMIO32(0xE000E180u + (irq / 32u) * 4u) = BIT(irq % 32u);
}

void Board_PinMode(uint32_t port, unsigned pin, uint32_t mode)
{
    volatile uint32_t *config = (volatile uint32_t *)(uintptr_t)(port + (pin >= 8u ? 4u : 0u));
    const unsigned shift = (pin % 8u) * 4u;
    *config = (*config & ~(15u << shift)) | (mode << shift);
}

void Board_PinWrite(uint32_t port, unsigned pin, bool high)
{
    MMIO32(port + (high ? 0x10u : 0x14u)) = BIT(pin);
}

bool Board_PinRead(uint32_t port, unsigned pin)
{
    return (MMIO32(port + 8) & BIT(pin)) != 0;
}

static void Clock_Init(void)
{
    MMIO32(RCC) |= BIT(0);
    MMIO32(RCC + 4) = 0;
    MMIO32(RCC) &= ~(BIT(24) | BIT(18));
    MMIO32(RCC) |= BIT(16);
    while (!(MMIO32(RCC) & BIT(17))) {
    }

    MMIO32(0x40022000u) = 0x11;
    MMIO32(RCC + 4) = BIT(16) | 0x400u; /* HSE direct, APB1 / 2. */
    MMIO32(RCC) = (MMIO32(RCC) & 0x000FFFFFu) | (5u << 26) | BIT(24);
    while (!(MMIO32(RCC) & BIT(25))) {
    }
    MMIO32(RCC + 4) |= 2u;
    while ((MMIO32(RCC + 4) & 0xCu) != 8u) {
    }
}

void Board_Init(void)
{
    Clock_Init();
    MMIO32(0xE000ED08u) = 0x08000000u;
    MMIO32(0xE000ED0Cu) = 0x05FA0500u;
    MMIO32(0xE000E014u) = 48000u - 1u;
    MMIO32(0xE000E018u) = 0;
    MMIO8(0xE000ED23u) = 0xF0;
    MMIO32(RCC + 0x18) |= BIT(0) | BIT(2) | BIT(3) | BIT(9) | BIT(10);
    MMIO32(RCC + 0x1C) |= BIT(0);
    MMIO32(0x40010004u) = 0x02000000u; /* Release PB4 from JTAG, retain SWD. */
    Board_PinMode(GPIOB, 0, 0);
    Board_PinMode(GPIOB, 1, 3);
    Board_PinWrite(GPIOB, 1, false);
    Board_PinWrite(GPIOB, 4, true);
    Board_PinWrite(GPIOB, 5, true);
    Board_PinMode(GPIOB, 4, 8);
    Board_PinMode(GPIOB, 5, 8);
    Board_PinMode(GPIOA, 8, 8);
    Board_PinWrite(GPIOA, 8, false);
    MMIO32(ADC1 + 4) = 0x200001F5u;
    MMIO32(ADC2 + 4) = 0x200001F5u;
    MMIO32(TIM2 + 0x28) = 479;
    MMIO32(TIM2 + 0x2C) = 9;
    MMIO32(TIM2 + 0x14) = 1;
    MMIO32(TIM2 + 0x10) = 0;
    MMIO32(TIM2 + 0x0C) = 1;
    Board_EnableIrq(28, 0xA0);
    MMIO32(TIM2) = 1;
    MMIO32(0xE000E010u) = 7u;
}

void SysTick_Handler(void)
{
    ++milliseconds;
    Links_CheckReceiveTimeouts(milliseconds);
    if (led_flash_requested) {
        led_flash_requested = false;
        if (!led_flash_ticks) {
            led_flash_ticks = 150;
        }
    }
    if (alarm_latched) {
        Board_PinWrite(GPIOB, 1, (milliseconds - alarm_latched_at) % 300u < 150u);
    } else {
        Board_PinWrite(GPIOB, 1, led_idle_high ^ (led_flash_ticks != 0));
    }
    if (led_flash_ticks) {
        --led_flash_ticks;
    }
}

uint32_t Board_Millis(void)
{
    return milliseconds;
}

void Board_Delay(uint32_t ms)
{
    uint32_t start = Board_Millis();
    while (Board_Millis() - start < ms) {
    }
}

static bool adc_sample(unsigned channel, uint32_t *result)
{
    MMIO32(ADC2 + 4) |= 7u << 10;
    MMIO32(ADC2 + 0x0C) = BIT(channel);
    MMIO32(ADC2 + 0x08) |= BIT(8);
    const uint32_t start = Board_Millis();
    while (!(MMIO32(ADC2 + 0x14) & 1u)) {
        if (Board_Millis() - start >= 3u) {
            return false;
        }
    }
    MMIO32(ADC2 + 0x14) = 1;
    *result = MMIO16(ADC2);
    return true;
}

static bool sample_battery(void)
{
    uint32_t reference = 0, input = 0, sample;
    for (unsigned i = 0; i < 4; ++i) {
        if (!adc_sample(8, &sample)) {
            return false;
        }
        reference += sample;
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (!adc_sample(0, &sample)) {
            return false;
        }
        input += sample;
    }
    if (!reference) {
        return false;
    }
    battery_mv = (uint16_t)((1200u * input) / reference);
    return true;
}

_Noreturn void Board_LatchAlarm(void)
{
    alarm_latched_at = Board_Millis();
    alarm_latched = true;
    for (;;) {
        __asm volatile("wfi");
    }
}

void Board_CheckStartupVoltage(void)
{
    sample_battery();
    if (!sample_battery() || battery_mv < CRITICAL_BATTERY_MV) {
        Board_LatchAlarm();
    }
    /* The original periodic check first runs around 400 ms after TIM2 starts. */
    next_battery_check = Board_Millis() + 250u;
}

uint16_t Board_Battery(void)
{
    return battery_mv;
}

void Board_Beep(unsigned pattern)
{
    if ((!pattern || Board_Millis() >= 500u) && beep_pattern != pattern) {
        beep_age = 0;
        beep_pattern = pattern;
    }
}

void Board_SerialError(void)
{
    if (Board_Millis() >= 500u) {
        serial_error_pending = true;
    }
}

void Board_FlashLed(void)
{
    led_flash_requested = true;
}

void Board_SetLedIdleLevel(bool high)
{
    led_idle_high = high;
}

static void buzzer_output(bool drive)
{
    if (drive == buzzer_driven) {
        return;
    }
    buzzer_driven = drive;
    if (drive) {
        Board_PinWrite(GPIOA, 8, true);
        Board_PinMode(GPIOA, 8, 3);
    } else {
        Board_PinMode(GPIOA, 8, 8);
        Board_PinWrite(GPIOA, 8, false);
    }
}

void TIM2_IRQHandler(void)
{
    MMIO32(TIM2 + 0x10) = 0;
    if (serial_error_pending) {
        serial_error_pending = false;
        if (!alarm_latched) {
            Board_Beep(2);
        }
    }
    if (alarm_latched) {
        Board_Beep(3);
    }
    const unsigned pattern = beep_pattern;
    if (!pattern) {
        buzzer_output(false);
        return;
    }
    const uint32_t age = beep_age++;
    bool sound = false;
    if (pattern == 1u) {
        sound = age < 1000u;
    } else if (pattern == 2u) {
        sound = age < 2400u && age % 1600u < 800u;
    } else {
        sound = age % 10000u < 5000u;
    }
    buzzer_output(sound && age % 6u == 0);
    if ((pattern == 1u && age >= 1000u) || (pattern == 2u && age >= 2400u)) {
        beep_pattern = 0;
    }
}

void Board_Service(void)
{
    const uint32_t now = Board_Millis();
    if ((int32_t)(now - next_battery_check) < 0) {
        return;
    }
    next_battery_check += BATTERY_CHECK_INTERVAL_MS;
    if (!sample_battery()) {
        low_battery_samples = critical_battery_samples = 0;
        return;
    }
    if (battery_mv >= LOW_BATTERY_MV) {
        low_battery_samples = 0;
        if (beep_pattern == 3u) {
            Board_Beep(0);
        }
    } else if (++low_battery_samples >= LOW_BATTERY_SAMPLES) {
        low_battery_samples = LOW_BATTERY_SAMPLES;
        if (!beep_pattern) {
            Board_Beep(3);
        }
    }
    if (battery_mv >= CRITICAL_BATTERY_MV) {
        critical_battery_samples = 0;
    } else if (++critical_battery_samples >= CRITICAL_BATTERY_SAMPLES) {
        Board_LatchAlarm();
    }
}
