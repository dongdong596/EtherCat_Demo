#include "swInput.h"
#include "gpio.h"

#define SWINPUT_ACTIVE_LOW  0U

volatile uint16_t g_dbg_swInputMask = 0;

uint16_t BSP_SWInput_ReadMask(void)
{
    uint16_t mask;
    uint16_t portB;
    uint16_t portC;

    portB = (uint16_t)((GPIOB->IDR >> 8) & 0x00FFU);  /* PB8~PB15 -> bit0~bit7 */
    portC = (uint16_t)(GPIOC->IDR & 0x00FFU);         /* PC0~PC7  -> bit8~bit15 */
    mask = (uint16_t)(portB | (uint16_t)(portC << 8));

#if SWINPUT_ACTIVE_LOW
    mask = (uint16_t)(~mask);
#endif

    g_dbg_swInputMask = mask;
    return mask;
}
