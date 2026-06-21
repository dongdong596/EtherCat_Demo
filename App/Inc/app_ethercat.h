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
 *  当前进度:
 *    已完成状态机、SyncManager 基础配置、CoE 邮箱和 CoE Online。
 *    下一阶段重点是 PDO 映射和 OP 态过程数据交换。
  ******************************************************************************
  */

#ifndef __APP_ETHERCAT_H__
#define __APP_ETHERCAT_H__

#include "AX58100.h"

#ifndef ECAT_DIAG_ENABLE
#define ECAT_DIAG_ENABLE 0
#endif

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

/* ── Watch 调试变量: 状态机和 SM0/SM1 邮箱配置 ── */
#if ECAT_DIAG_ENABLE
extern volatile uint8_t  g_dbg_alCtrlLo;
extern volatile uint8_t  g_dbg_alCtrlHi;
extern volatile uint8_t  g_dbg_alStatus;
extern volatile uint8_t  g_dbg_callCnt;
extern volatile uint16_t g_dbg_sm0Addr;
extern volatile uint16_t g_dbg_sm0Len;
extern volatile uint8_t  g_dbg_sm0Ctrl;
extern volatile uint16_t g_dbg_sm0Status;
extern volatile uint8_t  g_dbg_sm0Active;
extern volatile uint16_t g_dbg_sm1Addr;
extern volatile uint16_t g_dbg_sm1Len;
extern volatile uint8_t  g_dbg_sm1Ctrl;
extern volatile uint16_t g_dbg_sm1Status;
extern volatile uint8_t  g_dbg_sm1Active;
extern volatile uint32_t g_dbg_pdoOut;
extern volatile uint32_t g_dbg_pdoIn;
extern volatile uint16_t g_dbg_sm2Addr;
extern volatile uint16_t g_dbg_sm2Len;
extern volatile uint8_t  g_dbg_sm2Ctrl;
extern volatile uint8_t  g_dbg_sm2Active;
extern volatile uint16_t g_dbg_sm3Addr;
extern volatile uint16_t g_dbg_sm3Len;
extern volatile uint8_t  g_dbg_sm3Ctrl;
extern volatile uint8_t  g_dbg_sm3Active;
extern volatile uint16_t g_dbg_fmmuOutPhys;
extern volatile uint16_t g_dbg_fmmuOutLen;
extern volatile uint8_t  g_dbg_fmmuOutType;
extern volatile uint8_t  g_dbg_fmmuOutActive;
extern volatile uint16_t g_dbg_fmmuInPhys;
extern volatile uint16_t g_dbg_fmmuInLen;
extern volatile uint8_t  g_dbg_fmmuInType;
extern volatile uint8_t  g_dbg_fmmuInActive;
extern volatile uint16_t g_dbg_cfgError;
#endif

#ifdef __cplusplus
}
#endif

#endif /* __APP_ETHERCAT_H__ */
