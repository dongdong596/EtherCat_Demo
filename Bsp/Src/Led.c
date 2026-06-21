#include "Led.h"
#include "gpio.h"

volatile uint16_t g_dbg_ledMask = 0;

void BSP_LED_WriteMask(uint16_t mask)
{
    g_dbg_ledMask = mask;

    /* PE0~PE15 are dedicated LED outputs on this board. */
    GPIOE->ODR = (uint32_t)mask;
}

uint16_t BSP_LED_GetMask(void)
{
    return (uint16_t)g_dbg_ledMask;
}
