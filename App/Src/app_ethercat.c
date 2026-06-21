/**
  ******************************************************************************
  * @file    app_ethercat.c
  * @brief   EtherCAT 从站应用层实现
  *
  *  状态机协议 (Beckhoff ETG.1000):
  *    主站写 AL Control (0x0120) → 从站读 → 从站处理 → 从站写 AL Status (0x0130)
  *
  *  状态跳转图:
  *    (上电) → Init ⇄ PreOp ⇄ SafeOp ⇄ Op
  *                ↑        ↑        ↑
  *             可配寄存器  可邮箱    可过程数据
  ******************************************************************************
  */

#include "app_ethercat.h"
#include "app_coe.h"
#include <string.h>

/* ================================================================
 * 本地变量
 * ================================================================ */

static uint8_t m_currentState = ESC_STATE_INIT;  /* 当前 EtherCAT 状态 */
static uint8_t m_alError     = 0;                /* AL Status Code 低字节 */

/* Watch 调试变量: 状态机和邮箱 SM 配置 */
volatile uint8_t g_dbg_alCtrlLo = 0;  /* 最近一次读到的 AL Control [7:0]        */
volatile uint8_t g_dbg_alCtrlHi = 0;  /* 最近一次读到的 AL Control [15:8]       */
volatile uint8_t g_dbg_alStatus = 0;  /* 最近一次写入 AL Status [7:0]           */
volatile uint8_t g_dbg_callCnt  = 0;  /* ECAT_MainTask 调用次数 (溢出回绕)      */

/* SM0/SM1 邮箱诊断: 确认 TwinCAT 写入的邮箱窗口和控制字 */
volatile uint16_t g_dbg_sm0Addr   = 0;  /* SM0 实际物理起始地址                 */
volatile uint16_t g_dbg_sm0Len    = 0;  /* SM0 实际长度                          */
volatile uint8_t  g_dbg_sm0Ctrl   = 0;  /* SM0 control byte                      */
volatile uint16_t g_dbg_sm0Status = 0;  /* SM0 control/status word               */
volatile uint8_t  g_dbg_sm0Active = 0;  /* SM0 激活 (1=启用)                     */
volatile uint16_t g_dbg_sm1Addr   = 0;
volatile uint16_t g_dbg_sm1Len    = 0;
volatile uint8_t  g_dbg_sm1Ctrl   = 0;
volatile uint16_t g_dbg_sm1Status = 0;
volatile uint8_t  g_dbg_sm1Active = 0;
volatile uint32_t g_dbg_pdoOut    = 0;
volatile uint32_t g_dbg_pdoIn     = 0;

/* 过程数据缓冲区 (SM2 输出 主→从, SM3 输入 从→主) */
static uint8_t m_pdOutput[32] = {0};  /* SM2: 主站发来的数据 */
static uint8_t m_pdInput[32]  = {0};  /* SM3: 发给主站的数据 */

/* ================================================================
 * 内部辅助
 * ================================================================ */

static uint8_t IsValidTransition(uint8_t from, uint8_t to)
{
    if (to == ESC_STATE_INIT) return 1;           /* 任意状态可回 Init */
    switch (from)
    {
    case ESC_STATE_INIT:    return (to == ESC_STATE_PREOP);
    case ESC_STATE_PREOP:   return (to == ESC_STATE_SAFEOP || to == ESC_STATE_INIT);
    case ESC_STATE_SAFEOP:  return (to == ESC_STATE_OP || to == ESC_STATE_PREOP || to == ESC_STATE_INIT);
    case ESC_STATE_OP:      return (to == ESC_STATE_SAFEOP || to == ESC_STATE_INIT);
    default:                return 0;
    }
}

static void OnEnterState(uint8_t newState)
{
    switch (newState)
    {
    case ESC_STATE_INIT:
        m_alError = 0;
        break;
    case ESC_STATE_PREOP:
        /* SM 配置由主站通过网线写入 ESC 寄存器, MCU 不主动写
         * (参考 SSC: MBX_StartMailboxHandler 只读不写) */
        break;
    case ESC_STATE_SAFEOP:
        /* TODO 第5/6步: 验证 SM/FMMU 配置 */
        break;
    case ESC_STATE_OP:
        /* 初始化过程数据: 输入区填递增计数, 便于主站侧观察 */
        memset(m_pdInput,  0, sizeof(m_pdInput));
        memset(m_pdOutput, 0, sizeof(m_pdOutput));
        g_dbg_pdoOut = 0;
        g_dbg_pdoIn  = 0;
        break;
    }
}

static void OnLeaveState(uint8_t oldState)
{
    switch (oldState)
    {
    case ESC_STATE_OP:
        /* TODO 第7步: 冻结过程数据 */
        break;
    case ESC_STATE_SAFEOP:
        break;
    }
}

/* ================================================================
 * 核心跳转逻辑
 * ================================================================ */

/**
 * @brief  执行状态跳转 (可被 ECAT_MainTask / ECAT_SelfTest 复用)
 * @param  requestedState: 目标状态
 * @param  ackBit: AL Control bit4 — 主站状态切换确认标志
 *         主站握手协议: ACK=1 时从站须在 AL Status 设 Response 位 (bit4) 回应
 * @retval 跳转后的状态
 */
static uint8_t _ECAT_DoTransition(uint8_t requestedState, uint8_t ackBit)
{
    uint8_t statusLo;

    /* 状态没变: 回应当前状态 */
    if (requestedState == m_currentState)
    {
        statusLo = m_currentState;
        /* 不设 Response, TwinCAT 只要纯状态 */

        g_dbg_alStatus = statusLo;
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        return m_currentState;
    }

    /* 验证合法性 */
    if (!IsValidTransition(m_currentState, requestedState))
    {
        m_alError = 0x0011;
        statusLo = m_currentState | 0x20;
        /* 不设 Response, TwinCAT 只要纯状态 */
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, (uint8_t)(m_alError & 0xFF));
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, (uint8_t)(m_alError >> 8));
        return m_currentState;
    }

    /* 执行切换 */
    OnLeaveState(m_currentState);
    OnEnterState(requestedState);
    m_currentState = requestedState;
    m_alError      = AL_STATUS_NO_ERROR;

    /* Round 1: 写 AL Status, 镜像 ACK 位 */
    statusLo = m_currentState;
    if (ackBit) statusLo |= 0x10;

    g_dbg_alStatus = statusLo;
    ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
    ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, 0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, 0x00);

    return m_currentState;
}

/* ================================================================
 * 公开接口
 * ================================================================ */

void ECAT_Init(void)
{
    m_currentState = ESC_STATE_INIT;
    m_alError      = AL_STATUS_NO_ERROR;

    ESC_WriteRegister(ESC_REG_AL_STATUS,      ESC_STATE_INIT);
    ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, 0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, 0x00);
}

uint8_t ECAT_MainTask(void)
{
    uint8_t alCtrlLo, alCtrlHi, requestedState, ackBit, errAckBit;
    /* 读 AL Control (0x0120, 主站通过网线写, PDI 只读) */
    if (ESC_ReadRegister(ESC_REG_AL_CONTROL,     &alCtrlLo) != HAL_OK) return m_currentState;
    if (ESC_ReadRegister(ESC_REG_AL_CONTROL + 1, &alCtrlHi) != HAL_OK) return m_currentState;

    requestedState = alCtrlLo & 0x0F;
    ackBit         = alCtrlLo & 0x10;
    errAckBit      = alCtrlLo & 0x20;

    g_dbg_alCtrlLo = alCtrlLo;
    g_dbg_alCtrlHi = alCtrlHi;
    g_dbg_callCnt++;

    /* 诊断: 每周期回读 SM 配置 (持续更新, 观察主站写入时机) */
    if (m_currentState == ESC_STATE_PREOP ||
        m_currentState == ESC_STATE_SAFEOP ||
        m_currentState == ESC_STATE_OP)
    {	
        ECAT_DiagReadSM();
    }

    /* 主站清除错误 */
    if (errAckBit && m_alError != AL_STATUS_NO_ERROR)
    {
        m_alError = AL_STATUS_NO_ERROR;
        uint8_t statusLo = m_currentState;
        /* 不设 Response, TwinCAT 只要纯状态 */
        g_dbg_alStatus = statusLo;
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, 0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, 0x00);
        return m_currentState;
    }

    /* 过滤无效状态 */
    if (requestedState != ESC_STATE_INIT  &&
        requestedState != ESC_STATE_PREOP &&
        requestedState != ESC_STATE_BOOT  &&
        requestedState != ESC_STATE_SAFEOP &&
        requestedState != ESC_STATE_OP)
    {
        return m_currentState;
    }

    return _ECAT_DoTransition(requestedState, ackBit);
}

uint8_t ECAT_GetState(void)
{
    return m_currentState;
}

void ECAT_DiagReadSM(void)
{
    ESC_SM_Config_t cfg;

    ESC_SM_ReadConfig(0, &cfg);
    g_dbg_sm0Addr   = cfg.startAddr;
    g_dbg_sm0Len    = cfg.length;
    g_dbg_sm0Ctrl   = cfg.control;
    g_dbg_sm0Status = cfg.status;
    g_dbg_sm0Active = cfg.activate;

    ESC_SM_ReadConfig(1, &cfg);
    g_dbg_sm1Addr   = cfg.startAddr;
    g_dbg_sm1Len    = cfg.length;
    g_dbg_sm1Ctrl   = cfg.control;
    g_dbg_sm1Status = cfg.status;
    g_dbg_sm1Active = cfg.activate;
}

uint8_t ECAT_SelfTest(void)
{
    uint8_t errors = 0;
    uint8_t state;
    uint8_t i;

    static const uint8_t testSeq[] = {
        ESC_STATE_PREOP,
        ESC_STATE_SAFEOP,
        ESC_STATE_OP,
        ESC_STATE_SAFEOP,
        ESC_STATE_PREOP,
        ESC_STATE_INIT,
    };

    ECAT_Init();
    if (ECAT_GetState() != ESC_STATE_INIT) return 0xFF;

    for (i = 0; i < sizeof(testSeq); i++)
    {
        _ECAT_DoTransition(testSeq[i], 0x10);

        state = ECAT_GetState();
        if (state != testSeq[i]) errors |= (1 << i);

        uint8_t hwState = 0;
        ESC_ReadRegister(ESC_REG_AL_STATUS, &hwState);
        if ((hwState & 0x0F) != testSeq[i]) errors |= (1 << (i + 8));
    }

    ECAT_Init();
    return errors;
}

/* ================================================================
 * 过程数据交换 (OP 状态每周期调用)
 * ================================================================ */

void ECAT_ProcessDataExchange(void)
{
    if (m_currentState != ESC_STATE_OP) return;

    ESC_ReadOutputData(m_pdOutput, sizeof(m_pdOutput));

    g_dbg_pdoOut = ((uint32_t)m_pdOutput[0])
                 | ((uint32_t)m_pdOutput[1] << 8)
                 | ((uint32_t)m_pdOutput[2] << 16)
                 | ((uint32_t)m_pdOutput[3] << 24);
    g_testCounter = g_dbg_pdoOut;

    g_testStatus = g_testCounter + 1U;
    g_dbg_pdoIn = g_testStatus;

    m_pdInput[0] = (uint8_t)(g_testStatus);
    m_pdInput[1] = (uint8_t)(g_testStatus >> 8);
    m_pdInput[2] = (uint8_t)(g_testStatus >> 16);
    m_pdInput[3] = (uint8_t)(g_testStatus >> 24);

    ESC_WriteInputData(m_pdInput, sizeof(m_pdInput));
}
