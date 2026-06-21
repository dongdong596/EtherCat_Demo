#include "Led.h"
#include "gpio.h"

static uint16_t s_ledMask = 0;

void BSP_LED_WriteMask(uint16_t mask)
{
    s_ledMask = mask;

    /* PE0~PE15 are dedicated LED outputs on this board. */
    GPIOE->ODR = (uint32_t)mask;
}

uint16_t BSP_LED_GetMask(void)
{
    return s_ledMask;
}
