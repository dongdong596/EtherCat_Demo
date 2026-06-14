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

/* ================================================================
 * 本地变量
 * ================================================================ */

static uint8_t m_currentState = ESC_STATE_INIT;  /* 当前 EtherCAT 状态 */
static uint8_t m_alError     = 0;                /* AL Status Code 低字节 */

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

static void ECAT_SM_Init(void);  /* 前向声明, 实现在文件末尾 */

static void OnEnterState(uint8_t newState)
{
    switch (newState)
    {
    case ESC_STATE_INIT:
        m_alError = 0;
        break;
    case ESC_STATE_PREOP:
        /* 配默认 SM (无主站自测时使用; 有主站时主站会通过网线覆写) */
        ECAT_SM_Init();
        break;
    case ESC_STATE_SAFEOP:
        /* TODO 第5/6步: 验证 SM/FMMU 配置 */
        break;
    case ESC_STATE_OP:
        /* TODO 第7步: 激活过程数据交换 */
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
 * @retval 跳转后的状态
 */
static uint8_t _ECAT_DoTransition(uint8_t requestedState)
{
    uint8_t statusLo;

    /* 状态没变就不动 */
    if (requestedState == m_currentState)
        return m_currentState;

    /* 验证合法性 */
    if (!IsValidTransition(m_currentState, requestedState))
    {
        m_alError = 0x0011;  /* Invalid state change requested */
        ESC_WriteRegister(ESC_REG_AL_STATUS,      requestedState | 0x10);
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

    /* 写硬件 AL Status */
    statusLo = m_currentState;
    ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
    ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, 0x00);
    ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, 0x00);

    return m_currentState;
}

/* ================================================================
 * SyncManager 初始化
 * ================================================================ */

/**
 * @brief  配置默认 SM0~SM3 布局 (无主站自测用)
 *         有主站时主站会在 PreOp 阶段通过网线覆写
 *
 *  SM0: 0x1000, 128B, 邮箱 (主→从)
 *  SM1: 0x1080, 128B, 邮箱 (从→主)
 *  SM2: 0x1100,  32B, 缓冲 (主→从, 过程数据)
 *  SM3: 0x1120,  32B, 缓冲 (从→主, 过程数据)
 */
static void ECAT_SM_Init(void)
{
    ESC_WriteSMConfig(0, SM0_DEFAULT_ADDR, SM0_DEFAULT_LEN, SM0_DEFAULT_CTRL, 1);
    ESC_WriteSMConfig(1, SM1_DEFAULT_ADDR, SM1_DEFAULT_LEN, SM1_DEFAULT_CTRL, 1);
    ESC_WriteSMConfig(2, SM2_DEFAULT_ADDR, SM2_DEFAULT_LEN, SM2_DEFAULT_CTRL, 1);
    ESC_WriteSMConfig(3, SM3_DEFAULT_ADDR, SM3_DEFAULT_LEN, SM3_DEFAULT_CTRL, 1);
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
    uint8_t alCtrlLo, alCtrlHi, requestedState;

    /* 读 AL Control (0x0120, 主站通过网线写, PDI 只读) */
    if (ESC_ReadRegister(ESC_REG_AL_CONTROL,     &alCtrlLo) != HAL_OK) return m_currentState;
    if (ESC_ReadRegister(ESC_REG_AL_CONTROL + 1, &alCtrlHi) != HAL_OK) return m_currentState;

    requestedState = alCtrlLo & 0x0F;
    return _ECAT_DoTransition(requestedState);
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
        /* 直接调核心跳转逻辑 (跳过 AL Control 寄存器读写) */
        _ECAT_DoTransition(testSeq[i]);

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
