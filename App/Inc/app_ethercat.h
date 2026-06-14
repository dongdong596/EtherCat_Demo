/**
  ******************************************************************************
  * @file    app_ethercat.h
  * @brief   EtherCAT 从站应用层 — 状态机 / CoE / 过程数据
  * @author  dongdong596
  * @date    2026-06-14
  *
  *  层次:
  *    App/app_ethercat.c  →  EtherCAT 协议行为 (状态机 / CoE)
  *    BSP/AX58100.c       →  ESC 驱动 (寄存器读写 / SM 管理)
  *    Core/spi.c          →  SPI 硬件抽象
  *
  *  开发进度:
  *    ✅ 第4步: 状态机
  *    ✅ 第5步: SyncManager 配置 (迁移至 BSP 层 ESC_SM_Init)
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
 * 状态机 API
 * ================================================================ */

void    ECAT_Init(void);         /* 初始化状态机, 写 AL Status = Init      */
uint8_t ECAT_MainTask(void);     /* 状态机主循环: 读 AL Control → 跳转    */
uint8_t ECAT_GetState(void);     /* 返回当前 EtherCAT 状态                */
uint8_t ECAT_SelfTest(void);     /* 自测: 手动模拟全状态来回, 返回 0=通过 */

#ifdef __cplusplus
}
#endif

#endif /* __APP_ETHERCAT_H__ */
