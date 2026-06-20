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

void    ECAT_Init(void);                /* 初始化状态机, 写 AL Status = Init      */
uint8_t ECAT_MainTask(void);            /* 状态机主循环: 读 AL Control → 跳转    */
uint8_t ECAT_GetState(void);            /* 返回当前 EtherCAT 状态                */
uint8_t ECAT_SelfTest(void);            /* 自测: 手动模拟全状态来回, 返回 0=通过 */
void    ECAT_ProcessDataExchange(void);  /* 过程数据交换: OP 态每周期调用        */
void    ECAT_DiagReadSM(void);           /* 诊断: 回读 SM0/SM1 实际配置          */

/* ── 调试变量 (Watch 窗口直接观察) ── */
extern volatile uint8_t  g_dbg_alCtrlLo;
extern volatile uint8_t  g_dbg_alCtrlHi;
extern volatile uint8_t  g_dbg_alStatus;
extern volatile uint8_t  g_dbg_callCnt;
extern volatile uint8_t  g_dbg_pdiErr;
extern volatile uint16_t g_dbg_sm0Addr;
extern volatile uint16_t g_dbg_sm0Len;
extern volatile uint8_t  g_dbg_sm0Ctrl;
extern volatile uint8_t  g_dbg_sm0Status;
extern volatile uint8_t  g_dbg_sm0Active;
extern volatile uint16_t g_dbg_sm1Addr;
extern volatile uint8_t  g_dbg_sm1Ctrl;

/* ── AL Event Request (每次 SPI 事务自动更新) ── */
extern volatile uint8_t  g_dbg_irq0;
extern volatile uint8_t  g_dbg_irq1;
extern volatile uint8_t  g_dbg_sm1Status;
extern volatile uint8_t  g_dbg_sm1Active;

#ifdef __cplusplus
}
#endif

#endif /* __APP_ETHERCAT_H__ */
