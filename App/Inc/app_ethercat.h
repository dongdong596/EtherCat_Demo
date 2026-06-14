/**
  ******************************************************************************
  * @file    app_ethercat.h
  * @brief   EtherCAT 从站应用层 — 状态机 / SM / CoE / 过程数据
  * @author  dongdong596
  * @date    2026-06-14
  *
  *  层次:
  *    App/app_ethercat.c  →  EtherCAT 协议行为
  *    BSP/AX58100.c       →  ESC 驱动 (寄存器读写)
  *    Core/spi.c          →  SPI 硬件抽象
  *
  *  开发进度:
  *    ✅ 第4步: 状态机
  *    ✅ 第5步: SyncManager 配置
  *    ⬜ 第6步: CoE 邮箱协议
  *    ⬜ 第7步: 过程数据
  *    ⬜ 第8步: ESI/XML 从站描述文件
  ******************************************************************************
  */

#ifndef __APP_ETHERCAT_H__
#define __APP_ETHERCAT_H__

#include "AX58100.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SyncManager 默认布局 (无主站自测用, 上线后由主站覆写)
 * ================================================================
 *
 * RAM 用量: 128 + 128 + 32 + 32 = 320 字节 (0x1000~0x113F), 远小于 9KB
 */

#define SM0_DEFAULT_ADDR        0x1000U  /* 邮箱输出 (主→从)           */
#define SM0_DEFAULT_LEN         128U
#define SM0_DEFAULT_CTRL        SM_CTRL_M2S_MAILBOX

#define SM1_DEFAULT_ADDR        0x1080U  /* 邮箱输入 (从→主)           */
#define SM1_DEFAULT_LEN         128U
#define SM1_DEFAULT_CTRL        SM_CTRL_S2M_MAILBOX

#define SM2_DEFAULT_ADDR        0x1100U  /* 过程数据输出 (主→从)       */
#define SM2_DEFAULT_LEN         32U
#define SM2_DEFAULT_CTRL        SM_CTRL_M2S_BUFFERED

#define SM3_DEFAULT_ADDR        0x1120U  /* 过程数据输入 (从→主)       */
#define SM3_DEFAULT_LEN         32U
#define SM3_DEFAULT_CTRL        SM_CTRL_S2M_BUFFERED

/* ================================================================
 * 状态机
 * ================================================================ */

void    ECAT_Init(void);
uint8_t ECAT_MainTask(void);
uint8_t ECAT_GetState(void);
uint8_t ECAT_SelfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_ETHERCAT_H__ */
