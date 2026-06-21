#ifndef __SWINPUT_H__
#define __SWINPUT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint16_t BSP_SWInput_ReadMask(void);

extern volatile uint16_t g_dbg_swInputMask;

#ifdef __cplusplus
}
#endif

#endif /* __SWINPUT_H__ */
