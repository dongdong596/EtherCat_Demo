#ifndef __LED_H__
#define __LED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void BSP_LED_WriteMask(uint16_t mask);

#ifdef __cplusplus
}
#endif

#endif /* __LED_H__ */
