#ifndef __LED_H__
#define __LED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void BSP_LED_WriteMask(uint16_t mask);
uint16_t BSP_LED_GetMask(void);

extern volatile uint16_t g_dbg_ledMask;

#ifdef __cplusplus
}
#endif

#endif /* __LED_H__ */
