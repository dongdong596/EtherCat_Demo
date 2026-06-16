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
#include <string.h>

/* ================================================================
 * 本地变量
 * ================================================================ */

static uint8_t m_currentState = ESC_STATE_INIT;  /* 当前 EtherCAT 状态 */
static uint8_t m_alError     = 0;                /* AL Status Code 低字节 */

/* 调试变量 — Watch 窗口可直接观察 */
volatile uint8_t g_dbg_alCtrlLo = 0;  /* 最近一次读到的 AL Control [7:0]        */
volatile uint8_t g_dbg_alCtrlHi = 0;  /* 最近一次读到的 AL Control [15:8]       */
volatile uint8_t g_dbg_alStatus = 0;  /* 最近一次写入 AL Status [7:0]           */
volatile uint8_t g_dbg_callCnt  = 0;  /* ECAT_MainTask 调用次数 (溢出回绕)      */
volatile uint8_t g_dbg_pdiErr   = 0;  /* PDI 错误计数 (0x030D)                  */

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
        /* 配默认 SM (无主站自测时使用; 有主站时主站会通过网线覆写) */
        ESC_SM_Init();
        break;
    case ESC_STATE_SAFEOP:
        /* TODO 第5/6步: 验证 SM/FMMU 配置 */
        break;
    case ESC_STATE_OP:
        /* 初始化过程数据: 输入区填递增计数, 便于主站侧观察 */
        {
            static uint8_t s_toggle = 0;
            s_toggle = ~s_toggle;
            memset(m_pdInput,  s_toggle, sizeof(m_pdInput));
            memset(m_pdOutput, 0,        sizeof(m_pdOutput));
        }
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

    /* 状态没变: 仍须回应 ACK 握手 (主站可能重复请求确认) */
    if (requestedState == m_currentState)
    {
        statusLo = m_currentState;
        if (ackBit) statusLo |= 0x10;  /* Response 位 = ACK 位 */

        g_dbg_alStatus = statusLo;
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        return m_currentState;
    }

    /* 验证合法性 */
    if (!IsValidTransition(m_currentState, requestedState))
    {
        m_alError = 0x0011;  /* Invalid state change requested */
        /* AL Status: 保持当前状态 + Response 回应 + Error 标记 */
        statusLo = m_currentState | 0x20;  /* bit5 = Error */
        if (ackBit) statusLo |= 0x10;       /* bit4 = Response */
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

    /* 写硬件 AL Status: 当前状态 + Response 位镜像 ACK */
    statusLo = m_currentState;
    if (ackBit) statusLo |= 0x10;  /* Response 位 = ACK 位, 完成握手 */

    g_dbg_alStatus = statusLo;  /* 调试: 记录写入 AL Status 的值 */
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

    requestedState = alCtrlLo & 0x0F;   /* 目标状态  (bits 3:0) */
    ackBit         = alCtrlLo & 0x10;   /* ACK 握手位 (bit  4)  */
    errAckBit      = alCtrlLo & 0x20;   /* Error ACK  (bit  5)  */

    /* 更新调试变量 */
    g_dbg_alCtrlLo = alCtrlLo;
    g_dbg_alCtrlHi = alCtrlHi;
    g_dbg_callCnt++;

    /* 主站清除错误: 写 Error ACK → 从站复位错误标志 */
    if (errAckBit && m_alError != AL_STATUS_NO_ERROR)
    {
        m_alError = AL_STATUS_NO_ERROR;
        /* AL Status 去掉 Error 位但保持当前状态 */
        uint8_t statusLo = m_currentState;
        if (ackBit) statusLo |= 0x10;
        g_dbg_alStatus = statusLo;
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, 0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, 0x00);
        return m_currentState;
    }

    /* 过滤无效状态请求 (上电后主站尚未写入, AL Control 可能为 0) */
    if (requestedState != ESC_STATE_INIT  &&
        requestedState != ESC_STATE_PREOP &&
        requestedState != ESC_STATE_BOOT  &&
        requestedState != ESC_STATE_SAFEOP &&
        requestedState != ESC_STATE_OP)
    {
        return m_currentState;  /* 忽略, 不报错 */
    }

    return _ECAT_DoTransition(requestedState, ackBit);
}

uint8_t ECAT_GetState(void)
{
    return m_currentState;
}

/**
 * @brief  自测: 手动模拟主站切状态 (不需要网线)
 * @note   直接调用 _ECAT_DoTransition, 不通过 AL Control 寄存器
 *         AL Control (0x0120) 只能由主站通过网线写入, PDI 写不进去
 * @retval 0 = 全部通过, bit[7:0] 标记哪个跳转的软件状态不对,
 *         bit[15:8] 标记哪个跳转的硬件 AL Status 寄存器不对
 */
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

    /* 从 Init 开始 */
    ECAT_Init();
    if (ECAT_GetState() != ESC_STATE_INIT) return 0xFF;

    for (i = 0; i < sizeof(testSeq); i++)
    {
        /* 直接调核心跳转逻辑 (跳过 AL Control 寄存器读写, 模拟 ACK=1) */
        _ECAT_DoTransition(testSeq[i], 0x10);

        /* 检查软件状态 */
        state = ECAT_GetState();
        if (state != testSeq[i]) errors |= (1 << i);

        /* 检查 ESC 硬件 AL Status 寄存器也更新了 */
        uint8_t hwState = 0;
        ESC_ReadRegister(ESC_REG_AL_STATUS, &hwState);
        if ((hwState & 0x0F) != testSeq[i]) errors |= (1 << (i + 8));
    }

    /* 恢复 Init */
    ECAT_Init();

    return errors;  /* 0 = 全部通过 */
}

/* ================================================================
 * 过程数据交换 (OP 状态每周期调用)
 * ================================================================ */

/**
 * @brief  过程数据交换 — OP 状态下每周期执行一次
 * @note   1. 读 SM2 输出区 → m_pdOutput (主站发给从站的数据)
 *         2. 写 SM3 输入区 ← m_pdInput  (从站发给主站的数据)
 *         3. 更新 m_pdInput[0] 为递增值, 便于主站侧观察是否存活
 */
void ECAT_ProcessDataExchange(void)
{
    if (m_currentState != ESC_STATE_OP) return;

    /* 读主站输出 (SM2: M→S) */
    ESC_ReadOutputData(m_pdOutput, sizeof(m_pdOutput));

    /* 递增计数器, 主站可观察此值确认数据刷新 */
    m_pdInput[0]++;

    /* 写输入数据给主站 (SM3: S→M) */
    ESC_WriteInputData(m_pdInput, sizeof(m_pdInput));
}
