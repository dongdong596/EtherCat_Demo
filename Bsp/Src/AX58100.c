/**
  ******************************************************************************
  * @file    AX58100.c
  * @brief   AX58100 ESC 驱动实现
  *
  *  ╔══════════════════════════════════════════════════════════════╗
  *  ║  文件结构                                                     ║
  *  ║    §A  PDI 地址编码                                           ║
  *  ║    §B  PDI 传输层 — 寄存器读写                                 ║
  *  ║    §C  SyncManager 管理 — 配置 / 初始化 / 回读                 ║
  *  ║    §D  ESC 完整信息读取                                        ║
  *  ║    §E  测试 / 诊断                                            ║
  *  ╚══════════════════════════════════════════════════════════════╝
  *
  *  层次:
  *    ax58100.c  →  ESC PDI 协议 + 寄存器操作 + SM 管理 + 应用
  *    spi.c      →  SPI 硬件抽象 (CS 控制, 字节收发)
  ******************************************************************************
  */

#include "AX58100.h"

/* ================================================================
 * PDI 传输超时 (ms)
 * ================================================================ */
#define PDI_TIMEOUT_MS          100U

/* ================================================================
 * 全局变量 — 调试用, Watch 窗口直接观察
 * ================================================================ */
volatile uint8_t g_escType   = 0;    /* 最近一次读的 ESC 类型            */
volatile uint8_t g_escVer    = 0;    /* 最近一次读的 ESC 版本            */
volatile uint8_t g_escError  = 0;    /* 读写测试错误计数 (0 = 通过)      */
AX58100_Info_t    g_escInfo  = {0};  /* ESC 完整信息                     */

/* IRQ 状态缓存 (每次 SPI 事务自动更新) */
static uint8_t g_esc_irq0 = 0;       /* AL Event Request [7:0]  (0x0220) */
static uint8_t g_esc_irq1 = 0;       /* AL Event Request [15:8] (0x0221) */

/* ================================================================
 * §A  PDI 地址编码
 *     Beckhoff ESC SPI 2 字节地址模式:
 *       Byte0 = A[12:5]
 *       Byte1 = {A[4:0], CMD[2:0]}
 *     13 位地址覆盖 0x0000~0x1FFF, 3 位命令区分读/写/块读
 * ================================================================ */

/**
 * @brief  ESC 地址编码: 13 位地址 + 3 位命令 → 2 字节地址段
 * @param  addr: 13 位寄存器地址 (0x0000 ~ 0x1FFF)
 * @param  cmd:  3 位命令码 (ESC_CMD_READ / READ_NOWAIT / WRITE)
 * @param  pAddr0: 输出 Byte0 = A[12:5]
 * @param  pAddr1: 输出 Byte1 = {A[4:0], CMD[2:0]}
 */
static void ESC_EncodeAddress(uint16_t addr, uint8_t cmd,
                               uint8_t *pAddr0, uint8_t *pAddr1)
{
    *pAddr0 = (uint8_t)((addr >> 5) & 0xFF);               /* A[12:5] */
    *pAddr1 = (uint8_t)(((addr & 0x1F) << 3) | (cmd & 0x07)); /* A[4:0]<<3 | CMD */
}

/* ================================================================
 * §B  PDI 传输层  — ESC 寄存器读写
 * ================================================================ */

/**
 * @brief  ESC 读单个寄存器 (CMD=0x03, Read + Wait State)
 * @note   SPI 帧: {addr0, addr1, 0xFF(wait), 0xFF(read_term)} → 4 字节
 *         ESC 在第 2 字节返回 IRQ 状态, 第 4 字节返回寄存器数据
 */
HAL_StatusTypeDef ESC_ReadRegister(uint16_t addr, uint8_t *pData)
{
    HAL_StatusTypeDef status;
    uint8_t txBuf[4];       /* addr0, addr1, wait(0xFF), read_term(0xFF) */
    uint8_t rxBuf[4];
    uint8_t addr0, addr1;

    ESC_EncodeAddress(addr, ESC_CMD_READ, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    txBuf[2] = 0xFF;        /* wait state: 等待 ESC 内部读取 */
    txBuf[3] = 0xFF;        /* read termination */

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 4, PDI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];   /* 保存 IRQ 状态 */
        g_esc_irq1 = rxBuf[1];
        *pData = rxBuf[3];       /* 数据在第 4 个字节 */
    }
    return status;
}

/**
 * @brief  ESC 写单个寄存器 (CMD=0x04)
 * @note   SPI 帧: {addr0, addr1, data} → 3 字节
 */
HAL_StatusTypeDef ESC_WriteRegister(uint16_t addr, uint8_t data)
{
    HAL_StatusTypeDef status;
    uint8_t txBuf[3];       /* addr0, addr1, data */
    uint8_t rxBuf[3];
    uint8_t addr0, addr1;

    ESC_EncodeAddress(addr, ESC_CMD_WRITE, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    txBuf[2] = data;

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 3, PDI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
    }
    return status;
}

/**
 * @brief  ESC 读多个连续寄存器 (块读, CMD=0x02)
 * @note   拆为两次 SPI 传输: 地址段 → NOP 延时 → 数据段
 *         NOP 放在地址和数据之间, 满足 ESC t_read ≥ 240ns
 *         详见: 踩坑记录_NOP延时位置错误.md
 */
HAL_StatusTypeDef ESC_ReadBlock(uint16_t addr, uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    static uint8_t txBuf[2 + ESC_MAX_BLOCK_SIZE];
    static uint8_t rxBuf[2 + ESC_MAX_BLOCK_SIZE];
    uint16_t i;
    uint8_t addr0, addr1;

    if (size == 0 || size > ESC_MAX_BLOCK_SIZE) return HAL_ERROR;

    ESC_EncodeAddress(addr, ESC_CMD_READ_NOWAIT, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    for (i = 0; i < size - 1; i++)
    {
        txBuf[2 + i] = 0x00;        /* 非终止字节: MOSI=0x00, ESC 继续预取 */
    }
    txBuf[2 + size - 1] = 0xFF;     /* 最后一个: MOSI=0xFF, 读终止 */

    SPI_CS_LOW();

    /* 第一步: 只发地址段 (2 字节), ESC 收到地址后开始取数据 */
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2, PDI_TIMEOUT_MS);

    /* 地址和数据之间的延时: 满足 ESC t_read ≥ 240ns
     * STM32F103 @72MHz: 1 NOP ≈ 13.9ns, 10 NOP ≈ 139ns
     * 加上 HAL 函数返回开销和下一次调用准备, 总延时远超 240ns
     * 如需更高 SPI 频率, 增加 NOP 数量即可 */
    __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP(); __NOP();

    /* 第二步: 发数据段 (size 字节), ESC 已经有数据等着了 */
    if (status == HAL_OK)
    {
        status = HAL_SPI_TransmitReceive(&hspi1, txBuf + 2, rxBuf + 2, size, PDI_TIMEOUT_MS);
    }

    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
        for (i = 0; i < size; i++)
        {
            pData[i] = rxBuf[2 + i]; /* 数据从第 3 字节开始 (跳过地址段) */
        }
    }
    return status;
}

/**
 * @brief  ESC 写多个连续寄存器 (块写, CMD=0x04)
 */
HAL_StatusTypeDef ESC_WriteBlock(uint16_t addr, uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    static uint8_t txBuf[2 + ESC_MAX_BLOCK_SIZE];
    static uint8_t rxBuf[2 + ESC_MAX_BLOCK_SIZE];
    uint16_t i;
    uint8_t addr0, addr1;

    if (size > ESC_MAX_BLOCK_SIZE) return HAL_ERROR;

    ESC_EncodeAddress(addr, ESC_CMD_WRITE, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    for (i = 0; i < size; i++)
    {
        txBuf[2 + i] = pData[i];
    }

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2 + size, PDI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
    }
    return status;
}

/**
 * @brief  获取最近一次 SPI 事务的 ESC 中断请求状态
 * @note   ESC 在每次 SPI 地址段自动返回 AL Event Request 寄存器值
 *         (0x0220~0x0221), 本函数读取缓存而不发起新 SPI 事务
 */
void ESC_GetIRQStatus(uint8_t *irq0, uint8_t *irq1)
{
    *irq0 = g_esc_irq0;
    *irq1 = g_esc_irq1;
}

/* ================================================================
 * §C  SyncManager 管理
 *     ESC 有 8 个 SM 通道 (0~7), 每通道 16 字节配置空间.
 *
 *     典型分配:
 *       SM0: 邮箱 M→S  0x1000  128B  (主站 → 从站 邮箱命令)
 *       SM1: 邮箱 S→M  0x1080  128B  (从站 → 主站 邮箱响应)
 *       SM2: 缓冲 M→S  0x1100   32B  (主站 → 从站 过程数据)
 *       SM3: 缓冲 S→M  0x1120   32B  (从站 → 主站 过程数据)
 *       SM4~SM7: 保留, 未使用
 *
 *     注意:
 *       - 有主站时, SM 配置由主站通过网线在 PreOp 阶段写入
 *       - 以下函数仅用于无主站自测 / 调试
 * ================================================================ */

/**
 * @brief  用默认布局一键初始化 SM0~SM3 (无主站自测用)
 *
 *          SM0: 邮箱输出  0x1000  128B  M2S_MAILBOX
 *          SM1: 邮箱输入  0x1080  128B  S2M_MAILBOX
 *          SM2: 过程输出  0x1100   32B  M2S_BUFFERED
 *          SM3: 过程输入  0x1120   32B  S2M_BUFFERED
 */
void ESC_SM_Init(void)
{
    ESC_SM_Config_t cfg;

    /* SM0: 邮箱输出 (主→从) */
    cfg.startAddr = SM0_DEFAULT_ADDR;
    cfg.length    = SM0_DEFAULT_LEN;
    cfg.control   = SM0_DEFAULT_CTRL;
    cfg.activate  = 1;
    cfg.pdiCtrl   = 0;
    ESC_SM_Config(0, &cfg);

    /* SM1: 邮箱输入 (从→主) */
    cfg.startAddr = SM1_DEFAULT_ADDR;
    cfg.length    = SM1_DEFAULT_LEN;
    cfg.control   = SM1_DEFAULT_CTRL;
    cfg.activate  = 1;
    cfg.pdiCtrl   = 0;
    ESC_SM_Config(1, &cfg);

    /* SM2: 过程数据输出 (主→从, 缓冲模式) */
    cfg.startAddr = SM2_DEFAULT_ADDR;
    cfg.length    = SM2_DEFAULT_LEN;
    cfg.control   = SM2_DEFAULT_CTRL;
    cfg.activate  = 1;
    cfg.pdiCtrl   = 0;
    ESC_SM_Config(2, &cfg);

    /* SM3: 过程数据输入 (从→主, 缓冲模式) */
    cfg.startAddr = SM3_DEFAULT_ADDR;
    cfg.length    = SM3_DEFAULT_LEN;
    cfg.control   = SM3_DEFAULT_CTRL;
    cfg.activate  = 1;
    cfg.pdiCtrl   = 0;
    ESC_SM_Config(3, &cfg);
}

/**
 * @brief  读单个 SM 通道的完整配置到结构体
 * @param  smIdx: SM 索引 (0~7)
 * @param  pCfg:  输出 — 填充 6 字段 (含只读 status)
 * @note   使用块读, 一次 SPI 事务读回 8 字节
 */
void ESC_SM_ReadConfig(uint8_t smIdx, ESC_SM_Config_t *pCfg)
{
    uint16_t smBase = ESC_REG_SM_BASE + (uint16_t)smIdx * ESC_REG_SM_STRIDE;
    uint8_t buf[8];

    ESC_ReadBlock(smBase, buf, 8);

    pCfg->startAddr = (uint16_t)buf[SM_OFF_PHYS_START]
                    | ((uint16_t)buf[SM_OFF_PHYS_START + 1] << 8);
    pCfg->length    = (uint16_t)buf[SM_OFF_LENGTH]
                    | ((uint16_t)buf[SM_OFF_LENGTH + 1] << 8);
    pCfg->control   = buf[SM_OFF_CONTROL];
    pCfg->status    = buf[SM_OFF_STATUS];
    pCfg->activate  = buf[SM_OFF_ACTIVATE];
    pCfg->pdiCtrl   = buf[SM_OFF_PDI_CTRL];
}

/**
 * @brief  写单个 SM 通道的完整配置到 ESC
 * @param  smIdx: SM 索引 (0~7)
 * @param  pCfg:  要写入的配置
 * @note   逐寄存器写入 (6 次单寄存器 SPI 事务)
 *         不写 status 字段 (只读寄存器)
 */
void ESC_SM_Config(uint8_t smIdx, const ESC_SM_Config_t *pCfg)
{
    uint16_t smBase = ESC_REG_SM_BASE + (uint16_t)smIdx * ESC_REG_SM_STRIDE;

    ESC_WriteRegister(smBase + SM_OFF_PHYS_START,     (uint8_t)(pCfg->startAddr & 0xFF));
    ESC_WriteRegister(smBase + SM_OFF_PHYS_START + 1, (uint8_t)(pCfg->startAddr >> 8));
    ESC_WriteRegister(smBase + SM_OFF_LENGTH,         (uint8_t)(pCfg->length & 0xFF));
    ESC_WriteRegister(smBase + SM_OFF_LENGTH + 1,     (uint8_t)(pCfg->length >> 8));
    ESC_WriteRegister(smBase + SM_OFF_CONTROL,        pCfg->control);
    ESC_WriteRegister(smBase + SM_OFF_ACTIVATE,       pCfg->activate);
    ESC_WriteRegister(smBase + SM_OFF_PDI_CTRL,       pCfg->pdiCtrl);
}

/* ── 向后兼容接口 (内部转为新 API) ── */

/**
 * @brief  [兼容] 读 SM 通道配置 (旧接口, 逐参数)
 * @note   推荐使用 ESC_SM_ReadConfig() 新接口
 */
void ESC_ReadSMConfig(uint8_t smIdx, uint16_t *pStartAddr, uint16_t *pLength,
                      uint8_t *pControl, uint8_t *pStatus)
{
    ESC_SM_Config_t cfg;
    ESC_SM_ReadConfig(smIdx, &cfg);
    *pStartAddr = cfg.startAddr;
    *pLength    = cfg.length;
    *pControl   = cfg.control;
    if (pStatus) *pStatus = cfg.status;
}

/**
 * @brief  [兼容] 写 SM 通道配置 (旧接口, 逐参数)
 * @note   推荐使用 ESC_SM_Config() 新接口
 */
void ESC_WriteSMConfig(uint8_t smIdx, uint16_t startAddr, uint16_t length,
                       uint8_t control, uint8_t activate)
{
    ESC_SM_Config_t cfg;
    cfg.startAddr = startAddr;
    cfg.length    = length;
    cfg.control   = control;
    cfg.activate  = activate;
    cfg.pdiCtrl   = 0;
    ESC_SM_Config(smIdx, &cfg);
}

/* ================================================================
 * §D  ESC 完整信息读取
 * ================================================================ */

/**
 * @brief  读 ESC 完整设备信息
 * @note   一次调用读齐身份/能力/PDI 状态, 结果存入 g_escInfo
 *         只用 5 次 SPI 事务 (2 次块读 + 3 次单读), 通信开销极小
 */
HAL_StatusTypeDef AX58100_ReadESCInfo(void)
{
    HAL_StatusTypeDef status;
    uint8_t buf[16];

    /* ── 第1次: 块读身份信息 (0x0000~0x0009, 10 字节) ── */
    status = ESC_ReadBlock(ESC_REG_TYPE, buf, 10);
    if (status != HAL_OK) return status;

    g_escInfo.type    = buf[0];
    g_escInfo.revision = buf[1];
    /* buf[2], buf[3] reserved */
    g_escInfo.fmmuSupported = buf[4];
    g_escInfo.smSupported   = buf[5];
    g_escInfo.ramSizeKB     = buf[6];
    g_escInfo.portDesc      = buf[7];
    g_escInfo.features      = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);

    /* ── 第2次: 块读 MAC 地址 (0x0010~0x0015, 6 字节) ── */
    status = ESC_ReadBlock(ESC_REG_STATION_MAC, g_escInfo.mac, 6);
    if (status != HAL_OK) return status;

    /* ── PDI 配置 (单寄存器) ── */
    ESC_ReadRegister(ESC_REG_PDI_CONTROL, &g_escInfo.pdiControl);

    /* ── PDI 错误状态 (单寄存器) ── */
    ESC_ReadRegister(ESC_REG_PDI_ERR_CNT,  &g_escInfo.pdiErrorCnt);
    ESC_ReadRegister(ESC_REG_PDI_ERR_CODE, &g_escInfo.pdiErrorCode);

    /* 同步全局变量 (兼容旧接口 ESC_TestReadID) */
    g_escType = g_escInfo.type;
    g_escVer  = g_escInfo.revision;

    return HAL_OK;
}

/* ================================================================
 * §E  测试 / 诊断
 * ================================================================ */

/**
 * @brief  测试: 读 ESC 类型和版本寄存器
 * @note   单次执行后设断点观察 g_escType / g_escVer
 */
void ESC_TestReadID(void)
{
    uint8_t irq0, irq1;

    /* 读 ESC Type (0x0000) */
    g_escType = 0;
    if (ESC_ReadRegister(ESC_REG_TYPE, (uint8_t *)&g_escType) != HAL_OK)
    {
        g_escType = 0xFF;       /* 通信失败标记 */
    }

    /* 读 ESC Version (0x0001) */
    g_escVer = 0;
    if (ESC_ReadRegister(ESC_REG_REVISION, (uint8_t *)&g_escVer) != HAL_OK)
    {
        g_escVer = 0xFF;        /* 通信失败标记 */
    }

    /* IRQ 状态 */
    ESC_GetIRQStatus(&irq0, &irq1);

    __NOP();    /* 在这里设断点，Watch 窗口观察 g_escType / g_escVer */
    (void)irq0;
    (void)irq1;
}

/**
 * @brief  测试: 读写用户 RAM 区域
 * @note   写入测试数据到 0x1000 → 读回 → 比较
 *         g_escError == 0 表示通过
 */
void ESC_TestReadWrite(void)
{
    #define TEST_SIZE   8
    uint8_t txData[TEST_SIZE] = {0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x55, 0xAA};
    uint8_t rxData[TEST_SIZE];
    uint8_t i;

    g_escError = 0;

    /* 写测试数据到 RAM (0x1000) */
    if (ESC_WriteBlock(ESC_RAM_BASE, txData, TEST_SIZE) == HAL_OK)
    {
        HAL_Delay(1);

        /* 读回 */
        if (ESC_ReadBlock(ESC_RAM_BASE, rxData, TEST_SIZE) == HAL_OK)
        {
            for (i = 0; i < TEST_SIZE; i++)
            {
                if (rxData[i] != txData[i])
                {
                    g_escError++;
                }
            }
        }
        else
        {
            g_escError = 0xFF;      /* 读失败 */
        }
    }
    else
    {
        g_escError = 0xFE;          /* 写失败 */
    }

    __NOP();    /* 断点: g_escError==0 表示读写正确 */
}

/**
 * @brief  诊断函数: 一次性读出 ESC 关键信息
 * @note   设断点在最后的 __NOP(), Watch 窗口观察所有变量
 *         覆盖: 身份/PDI 错误/SM0 配置/RAM 内容/读写测试
 */
void ESC_Diagnose(void)
{
    /* ---- 基本信息 ---- */
    volatile uint8_t type     = 0;    /* ESC Type */
    volatile uint8_t rev      = 0;    /* ESC Revision */
    volatile uint8_t fmmu     = 0;    /* FMMU 数量 */
    volatile uint8_t sm       = 0;    /* SyncManager 数量 */
    volatile uint8_t ramSize  = 0;    /* RAM 大小(KB) */

    /* ---- PDI 错误诊断 ---- */
    volatile uint8_t pdiErrCnt  = 0;  /* PDI 错误计数 */
    volatile uint8_t pdiErrCode = 0;  /* 最后错误原因 */

    /* ---- SM0 配置 (新结构体) ---- */
    volatile ESC_SM_Config_t sm0Cfg = {0};

    /* ---- 0x1000 处现有数据 ---- */
    volatile uint8_t ram[8] = {0};    /* 原始数据 */

    /* ---- 单字节读写测试 ---- */
    volatile uint8_t testWr = 0xA5;
    volatile uint8_t testRd = 0;

    /* 读基本信息 */
    ESC_ReadRegister(ESC_REG_TYPE,           (uint8_t *)&type);
    ESC_ReadRegister(ESC_REG_REVISION,       (uint8_t *)&rev);
    ESC_ReadRegister(ESC_REG_FMMU_SUPPORTED, (uint8_t *)&fmmu);
    ESC_ReadRegister(ESC_REG_SM_SUPPORTED,   (uint8_t *)&sm);
    ESC_ReadRegister(ESC_REG_RAM_SIZE,       (uint8_t *)&ramSize);

    /* 读 PDI 错误计数 */
    ESC_ReadRegister(ESC_REG_PDI_ERR_CNT,  (uint8_t *)&pdiErrCnt);
    ESC_ReadRegister(ESC_REG_PDI_ERR_CODE, (uint8_t *)&pdiErrCode);

    /* 读 SM0 配置 (使用新 SM 管理接口) */
    ESC_SM_ReadConfig(0, (ESC_SM_Config_t *)&sm0Cfg);

    /* 读 0x1000 处原始数据 */
    ESC_ReadBlock(ESC_RAM_BASE, (uint8_t *)ram, 8);

    /* 单字节写 0x1F00 然后读回 (远离 SM 区域的高地址) */
    ESC_WriteRegister(0x1F00, 0xA5);
    ESC_ReadRegister (0x1F00, (uint8_t *)&testRd);

    __NOP();    /* 断点: 观察上面所有变量 */
}
