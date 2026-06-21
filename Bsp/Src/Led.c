#include "Led.h"
#include "gpio.h"

void BSP_LED_WriteMask(uint16_t mask)
{
    /* PE0~PE15 are dedicated LED outputs on this board. */
    GPIOE->ODR = (uint32_t)mask;
}

