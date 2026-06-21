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
#include "Led.h"
#include "swInput.h"
#include <string.h>

#define ECAT_PDO_BYTES                  4U
#define ECAT_AL_STATUS_ERROR            0x20U
#define ECAT_AL_INVALID_STATE_CHANGE    0x0011U
#define ECAT_AL_INVALID_SM_CONFIG       0x0017U
#define ECAT_AL_INVALID_OUTPUT_CONFIG   0x001DU
#define ECAT_AL_INVALID_INPUT_CONFIG    0x001EU
#define ECAT_AL_INVALID_INPUT_MAPPING   0x0024U
#define ECAT_AL_INVALID_OUTPUT_MAPPING  0x0025U

#define FMMU_TYPE_READ_ENABLE           0x01U
#define FMMU_TYPE_WRITE_ENABLE          0x02U
#define FMMU_MAX_CHECK                  4U

/* ================================================================
 * 本地变量
 * ================================================================ */

static uint8_t m_currentState = ESC_STATE_INIT;  /* 当前 EtherCAT 状态 */
static uint16_t m_alError    = 0;                /* AL Status Code */
static uint8_t m_skipConfigValidation = 0;

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
volatile uint16_t g_dbg_sm2Addr   = 0;
volatile uint16_t g_dbg_sm2Len    = 0;
volatile uint8_t  g_dbg_sm2Ctrl   = 0;
volatile uint8_t  g_dbg_sm2Active = 0;
volatile uint16_t g_dbg_sm3Addr   = 0;
volatile uint16_t g_dbg_sm3Len    = 0;
volatile uint8_t  g_dbg_sm3Ctrl   = 0;
volatile uint8_t  g_dbg_sm3Active = 0;
volatile uint16_t g_dbg_fmmuOutPhys = 0;
volatile uint16_t g_dbg_fmmuOutLen  = 0;
volatile uint8_t  g_dbg_fmmuOutType = 0;
volatile uint8_t  g_dbg_fmmuOutActive = 0;
volatile uint16_t g_dbg_fmmuInPhys = 0;
volatile uint16_t g_dbg_fmmuInLen  = 0;
volatile uint8_t  g_dbg_fmmuInType = 0;
volatile uint8_t  g_dbg_fmmuInActive = 0;
volatile uint16_t g_dbg_cfgError = 0;

/* 过程数据缓冲区 (SM2 输出 主→从, SM3 输入 从→主) */
static uint8_t m_pdOutput[32] = {0};  /* SM2: 主站发来的数据 */
static uint8_t m_pdInput[32]  = {0};  /* SM3: 发给主站的数据 */

typedef struct {
    uint32_t logicalStart;
    uint16_t length;
    uint16_t physicalStart;
    uint8_t  logicalStartBit;
    uint8_t  logicalStopBit;
    uint8_t  physicalStartBit;
    uint8_t  type;
    uint8_t  activate;
} ECAT_FMMU_Config_t;

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

static uint8_t SM_ControlMatches(uint8_t actual, uint8_t expected)
{
    return ((actual & 0x0FU) == (expected & 0x0FU)) ? 1U : 0U;
}

static void PublishSMProcessDiag(const ESC_SM_Config_t *sm2, const ESC_SM_Config_t *sm3)
{
    g_dbg_sm2Addr   = sm2->startAddr;
    g_dbg_sm2Len    = sm2->length;
    g_dbg_sm2Ctrl   = sm2->control;
    g_dbg_sm2Active = sm2->activate;

    g_dbg_sm3Addr   = sm3->startAddr;
    g_dbg_sm3Len    = sm3->length;
    g_dbg_sm3Ctrl   = sm3->control;
    g_dbg_sm3Active = sm3->activate;
}

static uint16_t CheckMailboxSMConfig(void)
{
    ESC_SM_Config_t sm0;
    ESC_SM_Config_t sm1;

    ESC_SM_ReadConfig(0, &sm0);
    ESC_SM_ReadConfig(1, &sm1);

    g_dbg_sm0Addr   = sm0.startAddr;
    g_dbg_sm0Len    = sm0.length;
    g_dbg_sm0Ctrl   = sm0.control;
    g_dbg_sm0Status = sm0.status;
    g_dbg_sm0Active = sm0.activate;

    g_dbg_sm1Addr   = sm1.startAddr;
    g_dbg_sm1Len    = sm1.length;
    g_dbg_sm1Ctrl   = sm1.control;
    g_dbg_sm1Status = sm1.status;
    g_dbg_sm1Active = sm1.activate;

    if (sm0.startAddr != SM0_DEFAULT_ADDR || sm0.length < 34U ||
        sm0.activate == 0U || !SM_ControlMatches(sm0.control, SM0_DEFAULT_CTRL))
    {
        return ECAT_AL_INVALID_SM_CONFIG;
    }

    if (sm1.startAddr != SM1_DEFAULT_ADDR || sm1.length < 34U ||
        sm1.activate == 0U || !SM_ControlMatches(sm1.control, SM1_DEFAULT_CTRL))
    {
        return ECAT_AL_INVALID_SM_CONFIG;
    }

    return AL_STATUS_NO_ERROR;
}

static uint16_t CheckProcessSMConfig(void)
{
    ESC_SM_Config_t sm2;
    ESC_SM_Config_t sm3;

    ESC_SM_ReadConfig(2, &sm2);
    ESC_SM_ReadConfig(3, &sm3);
    PublishSMProcessDiag(&sm2, &sm3);

    if (sm2.startAddr != SM2_DEFAULT_ADDR ||
        sm2.length < ECAT_PDO_BYTES ||
        sm2.length > sizeof(m_pdOutput) ||
        sm2.activate == 0U ||
        !SM_ControlMatches(sm2.control, SM2_DEFAULT_CTRL))
    {
        return ECAT_AL_INVALID_OUTPUT_CONFIG;
    }

    if (sm3.startAddr != SM3_DEFAULT_ADDR ||
        sm3.length < ECAT_PDO_BYTES ||
        sm3.length > sizeof(m_pdInput) ||
        sm3.activate == 0U ||
        !SM_ControlMatches(sm3.control, SM3_DEFAULT_CTRL))
    {
        return ECAT_AL_INVALID_INPUT_CONFIG;
    }

    return AL_STATUS_NO_ERROR;
}

static uint8_t ReadFMMUConfig(uint8_t index, ECAT_FMMU_Config_t *cfg)
{
    uint8_t buf[16];
    uint16_t addr = (uint16_t)(ESC_REG_FMMU_BASE + (uint16_t)index * ESC_REG_FMMU_STRIDE);

    if (cfg == NULL) return 0U;
    if (ESC_ReadBlock(addr, buf, sizeof(buf)) != HAL_OK) return 0U;

    cfg->logicalStart = ((uint32_t)buf[0])
                      | ((uint32_t)buf[1] << 8)
                      | ((uint32_t)buf[2] << 16)
                      | ((uint32_t)buf[3] << 24);
    cfg->length = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    cfg->logicalStartBit = buf[6];
    cfg->logicalStopBit = buf[7];
    cfg->physicalStart = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
    cfg->physicalStartBit = buf[10];
    cfg->type = buf[11];
    cfg->activate = buf[12];

    return 1U;
}

static uint16_t CheckFMMUConfig(void)
{
    uint8_t i;
    uint8_t foundOut = 0U;
    uint8_t foundIn = 0U;

    g_dbg_fmmuOutPhys = 0;
    g_dbg_fmmuOutLen = 0;
    g_dbg_fmmuOutType = 0;
    g_dbg_fmmuOutActive = 0;
    g_dbg_fmmuInPhys = 0;
    g_dbg_fmmuInLen = 0;
    g_dbg_fmmuInType = 0;
    g_dbg_fmmuInActive = 0;

    for (i = 0U; i < FMMU_MAX_CHECK; i++)
    {
        ECAT_FMMU_Config_t fmmu;
        if (!ReadFMMUConfig(i, &fmmu)) continue;
        if (fmmu.activate == 0U) continue;

        if (fmmu.physicalStart == SM2_DEFAULT_ADDR)
        {
            g_dbg_fmmuOutPhys = fmmu.physicalStart;
            g_dbg_fmmuOutLen = fmmu.length;
            g_dbg_fmmuOutType = fmmu.type;
            g_dbg_fmmuOutActive = fmmu.activate;

            if (fmmu.length >= ECAT_PDO_BYTES &&
                (fmmu.type & FMMU_TYPE_WRITE_ENABLE) != 0U)
            {
                foundOut = 1U;
            }
        }

        if (fmmu.physicalStart == SM3_DEFAULT_ADDR)
        {
            g_dbg_fmmuInPhys = fmmu.physicalStart;
            g_dbg_fmmuInLen = fmmu.length;
            g_dbg_fmmuInType = fmmu.type;
            g_dbg_fmmuInActive = fmmu.activate;

            if (fmmu.length >= ECAT_PDO_BYTES &&
                (fmmu.type & FMMU_TYPE_READ_ENABLE) != 0U)
            {
                foundIn = 1U;
            }
        }
    }

    if (!foundOut) return ECAT_AL_INVALID_OUTPUT_MAPPING;
    if (!foundIn) return ECAT_AL_INVALID_INPUT_MAPPING;

    return AL_STATUS_NO_ERROR;
}

static uint16_t ValidateTransitionConfig(uint8_t from, uint8_t to)
{
    uint16_t err;

    if (from == ESC_STATE_PREOP && to == ESC_STATE_SAFEOP)
    {
        err = CheckMailboxSMConfig();
        if (err != AL_STATUS_NO_ERROR) return err;

        return CheckProcessSMConfig();
    }

    if (from == ESC_STATE_SAFEOP && to == ESC_STATE_OP)
    {
        err = CheckProcessSMConfig();
        if (err != AL_STATUS_NO_ERROR) return err;

        return CheckFMMUConfig();
    }

    return AL_STATUS_NO_ERROR;
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
        /* 初始化过程数据缓存，OP 后由实际 IO 周期刷新 */
        memset(m_pdInput,  0, sizeof(m_pdInput));
        memset(m_pdOutput, 0, sizeof(m_pdOutput));
        g_dbg_pdoOut = 0;
        g_dbg_pdoIn  = 0;
        BSP_LED_WriteMask(0U);
        break;
    }
}

static void OnLeaveState(uint8_t oldState)
{
    switch (oldState)
    {
    case ESC_STATE_OP:
        /* TODO 第7步: 冻结过程数据 */
        BSP_LED_WriteMask(0U);
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
        m_alError = ECAT_AL_INVALID_STATE_CHANGE;
        g_dbg_cfgError = m_alError;
        statusLo = m_currentState | ECAT_AL_STATUS_ERROR;
        g_dbg_alStatus = statusLo;
        /* 不设 Response, TwinCAT 只要纯状态 */
        ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
        ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, (uint8_t)(m_alError & 0xFF));
        ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, (uint8_t)(m_alError >> 8));
        return m_currentState;
    }

    /* 执行切换 */
    if (!m_skipConfigValidation)
    {
        uint16_t cfgError = ValidateTransitionConfig(m_currentState, requestedState);
        if (cfgError != AL_STATUS_NO_ERROR)
        {
            m_alError = cfgError;
            g_dbg_cfgError = cfgError;
            statusLo = m_currentState | ECAT_AL_STATUS_ERROR;

            g_dbg_alStatus = statusLo;
            ESC_WriteRegister(ESC_REG_AL_STATUS,      statusLo);
            ESC_WriteRegister(ESC_REG_AL_STATUS + 1,  0x00);
            ESC_WriteRegister(ESC_REG_AL_STATUS_CODE, (uint8_t)(cfgError & 0xFF));
            ESC_WriteRegister(ESC_REG_AL_STATUS_CODE + 1, (uint8_t)(cfgError >> 8));
            return m_currentState;
        }
    }

    OnLeaveState(m_currentState);
    OnEnterState(requestedState);
    m_currentState = requestedState;
    m_alError      = AL_STATUS_NO_ERROR;
    g_dbg_cfgError = 0;

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
    g_dbg_cfgError = 0;

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
        g_dbg_cfgError = 0;
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

    {
        ESC_SM_Config_t sm2;
        ESC_SM_Config_t sm3;

        ESC_SM_ReadConfig(2, &sm2);
        ESC_SM_ReadConfig(3, &sm3);
        PublishSMProcessDiag(&sm2, &sm3);
    }
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

    m_skipConfigValidation = 1U;

    for (i = 0; i < sizeof(testSeq); i++)
    {
        _ECAT_DoTransition(testSeq[i], 0x10);

        state = ECAT_GetState();
        if (state != testSeq[i]) errors |= (1 << i);

        uint8_t hwState = 0;
        ESC_ReadRegister(ESC_REG_AL_STATUS, &hwState);
        if ((hwState & 0x0F) != testSeq[i]) errors |= (1 << (i + 8));
    }

    m_skipConfigValidation = 0U;
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
    BSP_LED_WriteMask((uint16_t)(g_testCounter & 0xFFFFU));

    g_testStatus = BSP_SWInput_ReadMask();
    g_dbg_pdoIn = g_testStatus;

    m_pdInput[0] = (uint8_t)(g_testStatus);
    m_pdInput[1] = (uint8_t)(g_testStatus >> 8);
    m_pdInput[2] = (uint8_t)(g_testStatus >> 16);
    m_pdInput[3] = (uint8_t)(g_testStatus >> 24);

    ESC_WriteInputData(m_pdInput, sizeof(m_pdInput));
}
