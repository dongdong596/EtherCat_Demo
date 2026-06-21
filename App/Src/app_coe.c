/**
  ******************************************************************************
  * @file    app_coe.c
  * @brief   CoE (CAN over EtherCAT) 协议栈实现 — SDO 服务 + 对象字典
  * @author  dongdong596
  * @date    2026-06-17
  *
  *  实现内容:
  *    1. 对象字典 (强制对象 + 应用对象)
  *    2. SDO Upload   (主站读对象)
  *    3. SDO Download (主站写对象)
  *    4. 邮箱通信主循环
  *
  *  使用方法:
  *    CoE_Init();               // 初始化 (一次)
  *    while (1) {
  *        CoE_MainTask();       // 轮询邮箱
  *        ECAT_MainTask();      // 状态机
  *    }
  ******************************************************************************
  */

#include "app_coe.h"
#include <string.h>

/* Watch 调试变量: 保留 CoE 在线诊断最常用的一组 */
volatile uint8_t  g_dbg_coe_rxCnt     = 0;  /* 收到 CoE 邮箱帧次数 */
volatile uint8_t  g_dbg_coe_procCnt   = 0;  /* 已处理 SDO/SDO Info 请求次数 */
volatile uint8_t  g_dbg_txTimeout     = 0;  /* 等待 SM1 发送邮箱空闲超时次数 */
volatile uint8_t  g_dbg_lastSvc       = 0;  /* 最近一次 CoE service */
volatile uint8_t  g_dbg_lastCmd       = 0;  /* 最近一次 SDO command */
volatile uint8_t  g_dbg_sdoInfoOp     = 0;  /* 最近一次 SDO Info opcode */
volatile uint16_t g_dbg_reqIndex      = 0;  /* 最近一次 SDO 请求 index */
volatile uint8_t  g_dbg_reqSubIndex   = 0;  /* 最近一次 SDO 请求 subindex */
volatile uint8_t  g_dbg_respCmd       = 0;  /* 最近一次 SDO 响应 command */
volatile uint16_t g_dbg_respIndex     = 0;  /* 最近一次 SDO 响应 index */
volatile uint8_t  g_dbg_respSubIndex  = 0;  /* 最近一次 SDO 响应 subindex */
volatile uint16_t g_dbg_lastTxLen     = 0;  /* 最近一次发送总长度, 含 6B 邮箱头 */
volatile uint16_t g_dbg_txMbxLen      = 0;  /* 最近一次 Mailbox Length 字段 */
static uint8_t g_tx_mbx_counter = 0;         /* 从站发送邮箱计数器 */
static uint16_t g_rx_mbx_address = 0;
static uint8_t g_rx_mbx_channel = 0;
static uint16_t g_rx_coe_header_low = 0;
static const uint8_t *g_sdo_seg_data = NULL;
static uint32_t g_sdo_seg_len = 0;
static uint32_t g_sdo_seg_off = 0;
static uint16_t g_sdo_seg_index = 0;
static uint8_t g_sdo_seg_subindex = 0;

/* ================================================================
 * §1  对象字典存储区
 * ================================================================ */

/* ── 必需对象数据 ── */
static uint32_t g_deviceType       = 0x00000000UL;  /* 0x1000: 设备类型 (0=通用) */
static char     g_deviceName[]     = "AX58100_Test";/* 0x1008: 设备名称         */
static char     g_hwVersion[]      = "1.0";         /* 0x1009: 硬件版本         */
static char     g_swVersion[]      = "1.0.0";       /* 0x100A: 软件版本         */

/* ── 0x1018: Identity Object (4 个子对象) ── */
static uint8_t  g_identity_maxSub  = 4;             /* 子索引 0: 最大子索引号   */
static uint32_t g_vendorID         = 0x00000596UL;  /* 子索引 1: 厂商 ID        */
static uint32_t g_productCode      = 0x58100000UL;  /* 子索引 2: 产品代码       */
static uint32_t g_revisionNumber   = 0x00020111UL;  /* 子索引 3: 版本号 (与XML/EEPROM一致) */
static uint32_t g_serialNumber     = 0x00000001UL;  /* 子索引 4: 序列号         */

/* ── 应用 IO 对象 (保留旧变量名，方便 Watch 继续使用) ── */
volatile uint32_t g_testCounter = 0;             /* 0x2000: LED Output Mask (rw)     */
volatile uint32_t g_testStatus  = 0;             /* 0x2001: Digital Input Mask (ro)  */

/* ── SM 通信类型 (0x1C00, 4 个子对象) ── */
static uint8_t  g_smType_maxSub    = 4;
static uint8_t  g_sm0_type         = 1;              /* SM0: 邮箱接收 (MbxRx)    */
static uint8_t  g_sm1_type         = 2;              /* SM1: 邮箱发送 (MbxTx)    */
static uint8_t  g_sm2_type         = 3;              /* SM2: 过程数据输出 (RxPDO) */
static uint8_t  g_sm3_type         = 4;              /* SM3: 过程数据输入 (TxPDO) */

/* ── PDO Assign (0x1C12 / 0x1C13) ── */
static uint8_t  g_rxPdoAssign      = 1;              /* 0x1C12:00 分配了 1 个 RxPDO */
static uint16_t g_rxPdoIdx         = 0x1600;         /* 0x1C12:01 → 0x1600         */
static uint8_t  g_txPdoAssign      = 1;              /* 0x1C13:00 分配了 1 个 TxPDO */
static uint16_t g_txPdoIdx         = 0x1A00;         /* 0x1C13:01 → 0x1A00         */

/* ── PDO 映射 (0x1600 RxPDO / 0x1A00 TxPDO) ── */
static uint8_t  g_rxPdoMapSub0     = 1;              /* 0x1600:00 映射条目数        */
static uint32_t g_rxPdoMapEntry    = 0x20000020UL;   /* 0x1600:01 → 0x2000:00,32bit */
static uint8_t  g_txPdoMapSub0     = 1;              /* 0x1A00:00 映射条目数        */
static uint32_t g_txPdoMapEntry    = 0x20010020UL;   /* 0x1A00:01 → 0x2001:00,32bit */

/* ================================================================
 * §2  对象字典定义 (OD_Entry_t 数组)
 * ================================================================ */

static OD_Entry_t g_objectDict[] = {
    /* ┌─────────────────────────────────────────────────────────┐
     * │ 强制对象 (ETG.1000.6 §5.6.7.4)                          │
     * └─────────────────────────────────────────────────────────┘ */

    /* 0x1000: Device Type */
    { 0x1000, 0, OD_TYPE_UINT32,     OD_ACCESS_RO, 0, 4, &g_deviceType },

    /* 0x1008: Device Name */
    { 0x1008, 0, OD_TYPE_VIS_STRING, OD_ACCESS_RO, 0, sizeof(g_deviceName)-1, g_deviceName },

    /* 0x1009: Hardware Version */
    { 0x1009, 0, OD_TYPE_VIS_STRING, OD_ACCESS_RO, 0, sizeof(g_hwVersion)-1, g_hwVersion },

    /* 0x100A: Software Version */
    { 0x100A, 0, OD_TYPE_VIS_STRING, OD_ACCESS_RO, 0, sizeof(g_swVersion)-1, g_swVersion },

    /* 0x1018: Identity Object (结构体, 5 个子索引) */
    { 0x1018, 0, OD_TYPE_UINT8,      OD_ACCESS_RO, 0, 1, &g_identity_maxSub },
    { 0x1018, 1, OD_TYPE_UINT32,     OD_ACCESS_RO, 0, 4, &g_vendorID },
    { 0x1018, 2, OD_TYPE_UINT32,     OD_ACCESS_RO, 0, 4, &g_productCode },
    { 0x1018, 3, OD_TYPE_UINT32,     OD_ACCESS_RO, 0, 4, &g_revisionNumber },
    { 0x1018, 4, OD_TYPE_UINT32,     OD_ACCESS_RO, 0, 4, &g_serialNumber },

    /* ┌─────────────────────────────────────────────────────────┐
     * │ 应用对象 (0x2000~0x5FFF: 厂商自定义区)                  │
     * └─────────────────────────────────────────────────────────┘ */

    /* ┌─────────────────────────────────────────────────────────┐
     * │ SM / PDO 对象 (ETG.1000.6, TwinCAT 必需)               │
     * └─────────────────────────────────────────────────────────┘ */

    /* 0x1C00: Sync Manager Communication Type */
    { 0x1C00, 0, OD_TYPE_UINT8, OD_ACCESS_RO, 0, 1, &g_smType_maxSub },
    { 0x1C00, 1, OD_TYPE_UINT8, OD_ACCESS_RO, 0, 1, &g_sm0_type },
    { 0x1C00, 2, OD_TYPE_UINT8, OD_ACCESS_RO, 0, 1, &g_sm1_type },
    { 0x1C00, 3, OD_TYPE_UINT8, OD_ACCESS_RO, 0, 1, &g_sm2_type },
    { 0x1C00, 4, OD_TYPE_UINT8, OD_ACCESS_RO, 0, 1, &g_sm3_type },

    /* 0x1600: RxPDO Mapping (1 条目 → 0x2000:00, 32bit) */
    { 0x1600, 0, OD_TYPE_UINT8,  OD_ACCESS_RO, 0, 1, &g_rxPdoMapSub0 },
    { 0x1600, 1, OD_TYPE_UINT32, OD_ACCESS_RO, 0, 4, &g_rxPdoMapEntry },

    /* 0x1A00: TxPDO Mapping (1 条目 → 0x2001:00, 32bit) */
    { 0x1A00, 0, OD_TYPE_UINT8,  OD_ACCESS_RO, 0, 1, &g_txPdoMapSub0 },
    { 0x1A00, 1, OD_TYPE_UINT32, OD_ACCESS_RO, 0, 4, &g_txPdoMapEntry },

    /* 0x1C12: RxPDO Assign */
    { 0x1C12, 0, OD_TYPE_UINT8,  OD_ACCESS_RO, 0, 1, &g_rxPdoAssign },
    { 0x1C12, 1, OD_TYPE_UINT16, OD_ACCESS_RO, 0, 2, &g_rxPdoIdx },

    /* 0x1C13: TxPDO Assign */
    { 0x1C13, 0, OD_TYPE_UINT8,  OD_ACCESS_RO, 0, 1, &g_txPdoAssign },
    { 0x1C13, 1, OD_TYPE_UINT16, OD_ACCESS_RO, 0, 2, &g_txPdoIdx },

    /* 0x2000: LED Output Mask (读写) */
    { 0x2000, 0, OD_TYPE_UINT32, OD_ACCESS_RW, 0, 4, (void *)&g_testCounter },

    /* 0x2001: Digital Input Mask (只读) */
    { 0x2001, 0, OD_TYPE_UINT32, OD_ACCESS_RO, 0, 4, (void *)&g_testStatus },
};

#define OD_SIZE  (sizeof(g_objectDict) / sizeof(g_objectDict[0]))

/* ================================================================
 * §3  内部辅助函数
 * ================================================================ */

/* ── 邮箱帧构造辅助 (避免 packed 结构体对齐问题) ── */

/** @brief 往 txBuf 写入 16-bit 小端序值 */
static void MBX_PutU16(uint8_t *buf, uint16_t offs, uint16_t val) {
    buf[offs]     = (uint8_t)(val & 0xFF);
    buf[offs + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/** @brief 往 txBuf 写入 32-bit 小端序值 */
static void MBX_PutU32(uint8_t *buf, uint16_t offs, uint32_t val) {
    buf[offs]     = (uint8_t)(val & 0xFF);
    buf[offs + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[offs + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[offs + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static uint16_t MBX_GetU16(const uint8_t *buf, uint16_t offs) {
    return (uint16_t)buf[offs] | ((uint16_t)buf[offs + 1] << 8);
}

static uint8_t MBX_MakeType(uint8_t type) {
    g_tx_mbx_counter++;
    if ((g_tx_mbx_counter & 0x07U) == 0) g_tx_mbx_counter = 1;
    return (uint8_t)((type & 0x0F) | (g_tx_mbx_counter << 4));
}

/** @brief 构造 CoE 响应头: 保留请求头低 12 位, 只替换 service[15:12] */
static uint16_t CoE_MakeResponseHeader(uint8_t service) {
    return (uint16_t)(g_rx_coe_header_low | ((uint16_t)service << 12));
}

/* ── 帧偏移常量 ── */
#define MBX_OFF_LENGTH      0
#define MBX_OFF_ADDRESS     2
#define MBX_OFF_CHANNEL     4
#define MBX_OFF_TYPE        5
#define MBX_HDR_SIZE        6
#define COE_OFF             6
#define COE_SIZE            2
#define SDO_OFF             8
#define SDO_CMD_OFF         8
#define SDO_IDX_OFF         9
#define SDO_SUB_OFF         11
#define SDO_DATA_OFF        12
#define SDO_HDR_SIZE        8
#define SDO_INFO_OFF        8
#define SDO_INFO_HEAD_SIZE  4
#define SDO_INFO_OPCODE_MASK 0x007FU

#define SDO_INFO_ACCESS_READ      0x0007U
#define SDO_INFO_ACCESS_WRITE     0x0038U
#define SDO_INFO_ACCESS_RXPDO     0x0040U
#define SDO_INFO_ACCESS_TXPDO     0x0080U

#define SDO_INFO_OBJCODE_VAR      0x07U
#define SDO_INFO_OBJCODE_RECORD   0x09U
#define SDO_INFO_LIST_TYPE_BACKUP 0x04U
#define SDO_INFO_LIST_TYPE_SET    0x05U

static uint8_t CoE_GetService(const uint8_t *buf) {
    return (uint8_t)((MBX_GetU16(buf, COE_OFF) >> 12) & 0x0F);
}

/**
 * @brief  在对象字典中查找条目
 * @param  index:    对象索引
 * @param  subindex: 子索引
 * @retval 找到返回条目指针, 否则返回 NULL
 */
static OD_Entry_t* OD_Find(uint16_t index, uint8_t subindex)
{
    uint16_t i;
    for (i = 0; i < OD_SIZE; i++)
    {
        if (g_objectDict[i].index == index && g_objectDict[i].subindex == subindex)
        {
            return &g_objectDict[i];
        }
    }
    return NULL;
}

static uint8_t OD_GetMaxSubIndex(uint16_t index)
{
    uint16_t i;
    uint8_t maxSub = 0;

    for (i = 0; i < OD_SIZE; i++)
    {
        if (g_objectDict[i].index == index && g_objectDict[i].subindex > maxSub)
        {
            maxSub = g_objectDict[i].subindex;
        }
    }

    return maxSub;
}

static uint8_t OD_GetObjectCode(uint16_t index)
{
    return (OD_GetMaxSubIndex(index) == 0) ? SDO_INFO_OBJCODE_VAR : SDO_INFO_OBJCODE_RECORD;
}

static uint8_t OD_GetObjectDataType(uint16_t index)
{
    OD_Entry_t *pEntry = OD_Find(index, 0);
    return (pEntry != NULL) ? pEntry->dataType : OD_TYPE_UINT32;
}

static uint16_t OD_GetAccessFlags(uint16_t index, uint8_t subindex)
{
    OD_Entry_t *pEntry = OD_Find(index, subindex);
    uint16_t flags = 0;

    if (pEntry == NULL) return 0;

    if (pEntry->access == OD_ACCESS_RO || pEntry->access == OD_ACCESS_RW)
    {
        flags |= SDO_INFO_ACCESS_READ;
    }
    if (pEntry->access == OD_ACCESS_WO || pEntry->access == OD_ACCESS_RW)
    {
        flags |= SDO_INFO_ACCESS_WRITE;
    }

    if (index == 0x2000 || index == 0x1600 || index == 0x1C12)
    {
        flags |= SDO_INFO_ACCESS_RXPDO;
    }
    if (index == 0x2001 || index == 0x1A00 || index == 0x1C13)
    {
        flags |= SDO_INFO_ACCESS_TXPDO;
    }

    return flags;
}

static uint16_t OD_GetBitLength(uint16_t index, uint8_t subindex)
{
    OD_Entry_t *pEntry = OD_Find(index, subindex);
    return (pEntry != NULL) ? (uint16_t)(pEntry->dataLen * 8U) : 0;
}

static void SDO_InfoWriteHeader(uint8_t *txBuf, uint8_t opCode, uint16_t fragmentsLeft)
{
    MBX_PutU16(txBuf, SDO_INFO_OFF + 0, opCode & SDO_INFO_OPCODE_MASK);
    MBX_PutU16(txBuf, SDO_INFO_OFF + 2, fragmentsLeft);
}

static void SDO_InfoSendError(uint32_t abortCode)
{
    uint8_t txBuf[128];
    uint16_t dataLen = COE_SIZE + SDO_INFO_HEAD_SIZE + 4U;
    uint16_t timeout = 1000;

    memset(txBuf, 0, sizeof(txBuf));
    MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
    MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
    txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
    txBuf[MBX_OFF_TYPE] = MBX_MakeType(MBX_TYPE_COE);
    MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_INFO));
    SDO_InfoWriteHeader(txBuf, SDO_INFO_OPCODE_ERROR, 0);
    MBX_PutU32(txBuf, SDO_INFO_OFF + SDO_INFO_HEAD_SIZE, abortCode);

    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }
    if (timeout == 0) {
        g_dbg_txTimeout++;
        return;
    }

    g_dbg_lastTxLen = MBX_HDR_SIZE + dataLen;
    g_dbg_txMbxLen = dataLen;
    g_dbg_respCmd = 0;
    g_dbg_respIndex = 0;
    g_dbg_respSubIndex = 0;
    ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
}

/**
 * @brief  发送 SDO Abort 响应
 * @param  index:     对象索引
 * @param  subindex:  子索引
 * @param  abortCode: SDO 错误码
 */
static void SDO_SendAbort(uint16_t index, uint8_t subindex, uint32_t abortCode)
{
    uint8_t txBuf[128];
    memset(txBuf, 0, sizeof(txBuf));

    uint16_t dataLen = COE_SIZE + SDO_HDR_SIZE;  /* 2 + 8 = 10 */

    /* 邮箱头 */
    MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
    MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
    txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
    txBuf[MBX_OFF_TYPE]    = MBX_MakeType(MBX_TYPE_COE);

    /* CoE 头 */
    MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_RESPONSE));

    /* SDO Abort */
    txBuf[SDO_CMD_OFF] = SDO_CMD_ABORT;
    MBX_PutU16(txBuf, SDO_IDX_OFF, index);
    txBuf[SDO_SUB_OFF] = subindex;
    MBX_PutU32(txBuf, SDO_DATA_OFF, abortCode);

    /* 等待上次响应被取走 */
    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    if (timeout == 0) {
        g_dbg_txTimeout++;  /* 记录超时次数 */
        return;             /* 超时则放弃本次响应，避免覆盖上一个 */
    }

    g_dbg_txMbxLen = dataLen;
    g_dbg_respCmd = txBuf[SDO_CMD_OFF];
    g_dbg_respIndex = index;
    g_dbg_respSubIndex = subindex;
    ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
}

/**
 * @brief  处理 SDO Upload 请求 (主站读对象)
 * @param  index:    对象索引
 * @param  subindex: 子索引
 */
static void SDO_HandleUpload(uint16_t index, uint8_t subindex)
{
    OD_Entry_t *pEntry = OD_Find(index, subindex);

    if (pEntry == NULL) {
        SDO_SendAbort(index, subindex, SDO_ABORT_NOT_EXIST);
        return;
    }
    if (pEntry->access == OD_ACCESS_WO) {
        SDO_SendAbort(index, subindex, SDO_ABORT_WRITEONLY);
        return;
    }

    uint8_t txBuf[128];
    memset(txBuf, 0, sizeof(txBuf));
    uint16_t dataLen;

    if (pEntry->dataLen <= 4)
    {
        dataLen = COE_SIZE + SDO_HDR_SIZE;  /* 2 + 8 = 10 */

        /* 邮箱头 (6 B) */
        MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
        MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
        txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
        txBuf[MBX_OFF_TYPE]    = MBX_MakeType(MBX_TYPE_COE);

        /* CoE 头 (2 B): service=SDO_RESPONSE(3), number=0 */
        MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_RESPONSE));

        /* SDO Upload Expedited */
        txBuf[SDO_CMD_OFF] = SDO_CMD_UPLOAD_EXPEDITED | ((4 - pEntry->dataLen) << 2);
        MBX_PutU16(txBuf, SDO_IDX_OFF, index);
        txBuf[SDO_SUB_OFF] = subindex;
        /* data 域: 小端序拷贝 */
        memcpy(txBuf + SDO_DATA_OFF, pEntry->pData, pEntry->dataLen);
    }
    else
    {
        if (pEntry->dataLen > (sizeof(txBuf) - MBX_HDR_SIZE - COE_SIZE - SDO_HDR_SIZE)) {
            SDO_SendAbort(index, subindex, SDO_ABORT_INTERNAL);
            return;
        }

        dataLen = COE_SIZE + SDO_HDR_SIZE;

        MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
        MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
        txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
        txBuf[MBX_OFF_TYPE]    = MBX_MakeType(MBX_TYPE_COE);

        MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_RESPONSE));

        /* SDO Upload Normal */
        txBuf[SDO_CMD_OFF] = SDO_CMD_UPLOAD_NORMAL;
        MBX_PutU16(txBuf, SDO_IDX_OFF, index);
        txBuf[SDO_SUB_OFF] = subindex;
        MBX_PutU32(txBuf, SDO_DATA_OFF, pEntry->dataLen);

        g_sdo_seg_data = (const uint8_t *)pEntry->pData;
        g_sdo_seg_len = pEntry->dataLen;
        g_sdo_seg_off = 0;
        g_sdo_seg_index = index;
        g_sdo_seg_subindex = subindex;
    }

    /* 等待上次响应被取走 */
    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    if (timeout == 0) {
        g_dbg_txTimeout++;  /* 记录超时次数 */
        return;             /* 超时则放弃本次响应，避免覆盖上一个 */
    }

    g_dbg_lastTxLen = MBX_HDR_SIZE + dataLen;
    g_dbg_txMbxLen = dataLen;
    g_dbg_respCmd = txBuf[SDO_CMD_OFF];
    g_dbg_respIndex = index;
    g_dbg_respSubIndex = subindex;
    ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
}

static void SDO_HandleUploadSegment(uint8_t reqCmd)
{
    uint8_t txBuf[128];
    uint16_t dataLen = COE_SIZE + 1U + 7U;
    uint16_t timeout = 1000;
    uint32_t remain;
    uint8_t sendLen;
    uint8_t segCmd;

    if (g_sdo_seg_data == NULL || g_sdo_seg_off >= g_sdo_seg_len)
    {
        SDO_SendAbort(g_sdo_seg_index, g_sdo_seg_subindex, SDO_ABORT_CMD_INVALID);
        return;
    }

    memset(txBuf, 0, sizeof(txBuf));

    remain = g_sdo_seg_len - g_sdo_seg_off;
    sendLen = (remain > 7U) ? 7U : (uint8_t)remain;

    MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
    MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
    txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
    txBuf[MBX_OFF_TYPE] = MBX_MakeType(MBX_TYPE_COE);
    MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_RESPONSE));

    segCmd = (uint8_t)(SDO_CMD_UPLOAD_SEG_RESP | (reqCmd & 0x10U));
    if (sendLen < 7U)
    {
        segCmd |= (uint8_t)((7U - sendLen) << 1);
    }
    if (remain <= 7U)
    {
        segCmd |= 0x01U;
    }

    txBuf[SDO_OFF] = segCmd;
    memcpy(txBuf + SDO_OFF + 1, g_sdo_seg_data + g_sdo_seg_off, sendLen);

    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }
    if (timeout == 0) {
        g_dbg_txTimeout++;
        return;
    }

    g_dbg_lastTxLen = MBX_HDR_SIZE + dataLen;
    g_dbg_txMbxLen = dataLen;
    g_dbg_respCmd = txBuf[SDO_OFF];
    g_dbg_respIndex = g_sdo_seg_index;
    g_dbg_respSubIndex = g_sdo_seg_subindex;
    ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);

    g_sdo_seg_off += sendLen;
    if (g_sdo_seg_off >= g_sdo_seg_len)
    {
        g_sdo_seg_data = NULL;
        g_sdo_seg_len = 0;
        g_sdo_seg_off = 0;
    }
}

/**
 * @brief  处理 SDO Download 请求 (主站写对象)
 * @param  pSDO: SDO 请求头指针
 */
static void SDO_HandleDownload(SDO_Header_t *pSDO)
{
    uint16_t index    = pSDO->index;
    uint8_t  subindex = pSDO->subindex;

    OD_Entry_t *pEntry = OD_Find(index, subindex);

    if (pEntry == NULL) {
        SDO_SendAbort(index, subindex, SDO_ABORT_NOT_EXIST);
        return;
    }
    if (pEntry->access == OD_ACCESS_RO) {
        SDO_SendAbort(index, subindex, SDO_ABORT_READONLY);
        return;
    }

    uint8_t cmd = pSDO->command;
    if (cmd & 0x02)
    {
        uint8_t dataLen = 4 - ((cmd >> 2) & 0x03);
        if (dataLen > pEntry->dataLen) {
            SDO_SendAbort(index, subindex, SDO_ABORT_LEN_MISMATCH);
            return;
        }
        memcpy(pEntry->pData, (const void *)&pSDO->data, dataLen);
    }
    else
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_UNSUPPORTED);
        return;
    }

    /* 发送确认 — 直接字节写入 */
    uint8_t txBuf[128];
    memset(txBuf, 0, sizeof(txBuf));

    uint16_t dataLen = COE_SIZE + SDO_HDR_SIZE;

    MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
    MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
    txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
    txBuf[MBX_OFF_TYPE]    = MBX_MakeType(MBX_TYPE_COE);

    MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_RESPONSE));

    txBuf[SDO_CMD_OFF] = SDO_CMD_DOWNLOAD_RESP;
    MBX_PutU16(txBuf, SDO_IDX_OFF, index);
    txBuf[SDO_SUB_OFF] = subindex;
    /* data = 0 (already zeroed by memset) */

    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    if (timeout == 0) {
        g_dbg_txTimeout++;  /* 记录超时次数 */
        return;             /* 超时则放弃本次响应，避免覆盖上一个 */
    }

    ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
}

/**
 * @brief  处理 SDO Info 请求 (主站查询对象字典结构)
 * @param  rxBuf: 接收缓冲区（包含完整邮箱帧）
 * @note   SDO Info 用于 TwinCAT 扫描对象字典，返回对象列表
 */
static void SDO_HandleInfo(uint8_t *rxBuf)
{
    uint8_t txBuf[128];
    memset(txBuf, 0, sizeof(txBuf));

    /* SDO Info 请求格式：CoE头后第1字节是OpCode */
    uint16_t infoHead = MBX_GetU16(rxBuf, SDO_INFO_OFF);
    uint8_t opCode = (uint8_t)(infoHead & SDO_INFO_OPCODE_MASK);
    g_dbg_sdoInfoOp = opCode;  /* 记录OpCode用于调试 */

    if (opCode == SDO_INFO_OPCODE_LIST_REQ)
    {
        /* Get OD List Request - 返回对象字典的索引列表 */
        /* 构建对象索引列表 (只列出主索引，不含子索引) */
        uint16_t objList[] = {
            0x1000, 0x1008, 0x1009, 0x100A, 0x1018,
            0x1600, 0x1A00, 0x1C00, 0x1C12, 0x1C13,
            0x2000, 0x2001
        };
        uint16_t rxPdoList[] = { 0x1600, 0x1C12, 0x2000 };
        uint16_t txPdoList[] = { 0x1A00, 0x1C13, 0x2001 };
        uint16_t objCount = sizeof(objList) / sizeof(objList[0]);
        uint16_t *listPtr = objList;
        uint16_t listCount = objCount;
        /* SDO Info List Response: InfoHead(2) + FragmentsLeft(2) + ListType(2) + List */
        uint16_t listType = MBX_GetU16(rxBuf, SDO_INFO_OFF + SDO_INFO_HEAD_SIZE);
        uint16_t dataLen;
        if (listType > SDO_INFO_LIST_TYPE_SET)
        {
            SDO_InfoSendError(SDO_ABORT_UNSUPPORTED);
            return;
        }

        if (listType == SDO_INFO_LIST_TYPE_RXPDO)
        {
            listPtr = rxPdoList;
            listCount = sizeof(rxPdoList) / sizeof(rxPdoList[0]);
        }
        else if (listType == SDO_INFO_LIST_TYPE_TXPDO)
        {
            listPtr = txPdoList;
            listCount = sizeof(txPdoList) / sizeof(txPdoList[0]);
        }
        else if (listType == SDO_INFO_LIST_TYPE_BACKUP || listType == SDO_INFO_LIST_TYPE_SET)
        {
            listCount = 0;
        }

        dataLen = COE_SIZE + SDO_INFO_HEAD_SIZE;
        dataLen += (uint16_t)(2U + ((listType == SDO_INFO_LIST_TYPE_LENGTH) ? 10U : (listCount * 2U)));

        /* 邮箱头 */
        MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
        MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
        txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
        txBuf[MBX_OFF_TYPE] = MBX_MakeType(MBX_TYPE_COE);

        /* CoE 头 */
        MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_INFO));

        SDO_InfoWriteHeader(txBuf, SDO_INFO_OPCODE_LIST_RESP, 0);

        uint16_t offset = SDO_INFO_OFF + SDO_INFO_HEAD_SIZE;

        if (listType == SDO_INFO_LIST_TYPE_LENGTH)
        {
            MBX_PutU16(txBuf, offset + 0, listType);
            MBX_PutU16(txBuf, offset + 2, objCount);
            MBX_PutU16(txBuf, offset + 4, (uint16_t)(sizeof(rxPdoList) / sizeof(rxPdoList[0])));
            MBX_PutU16(txBuf, offset + 6, (uint16_t)(sizeof(txPdoList) / sizeof(txPdoList[0])));
            MBX_PutU16(txBuf, offset + 8, 0);
            MBX_PutU16(txBuf, offset + 10, 0);
        }
        else
        {
            MBX_PutU16(txBuf, offset, listType);
            for (uint16_t i = 0; i < listCount; i++)
            {
                MBX_PutU16(txBuf, offset + 2 + i * 2, listPtr[i]);
            }
        }

        /* 等待发送 */
        uint16_t timeout = 1000;
        while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }
        if (timeout == 0) {
            g_dbg_txTimeout++;
            return;
        }

        g_dbg_lastTxLen = MBX_HDR_SIZE + dataLen;
        ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
    }
    else if (opCode == SDO_INFO_OPCODE_OBJ_REQ)
    {
        /* Get Object Description Request */
        uint16_t reqIndex = MBX_GetU16(rxBuf, SDO_INFO_OFF + SDO_INFO_HEAD_SIZE);

        if (OD_Find(reqIndex, 0) == NULL)
        {
            SDO_InfoSendError(SDO_ABORT_NOT_EXIST);
            return;
        }

        uint16_t dataLen = COE_SIZE + SDO_INFO_HEAD_SIZE + 6U;

        MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
        MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
        txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
        txBuf[MBX_OFF_TYPE] = MBX_MakeType(MBX_TYPE_COE);

        MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_INFO));

        SDO_InfoWriteHeader(txBuf, SDO_INFO_OPCODE_OBJ_RESP, 0);

        uint16_t offset = SDO_INFO_OFF + SDO_INFO_HEAD_SIZE;
        MBX_PutU16(txBuf, offset + 0, reqIndex);
        MBX_PutU16(txBuf, offset + 2, OD_GetObjectDataType(reqIndex));
        MBX_PutU16(txBuf, offset + 4,
                   (uint16_t)OD_GetMaxSubIndex(reqIndex) |
                   ((uint16_t)OD_GetObjectCode(reqIndex) << 8));

        uint16_t timeout = 1000;
        while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }
        if (timeout == 0) {
            g_dbg_txTimeout++;
            return;
        }

        ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
    }
    else if (opCode == SDO_INFO_OPCODE_ENTRY_REQ)
    {
        /* Get Entry Description Request */
        uint16_t reqIndex = MBX_GetU16(rxBuf, SDO_INFO_OFF + SDO_INFO_HEAD_SIZE);
        uint8_t reqSub = rxBuf[SDO_INFO_OFF + SDO_INFO_HEAD_SIZE + 2];
        uint8_t valueInfo = rxBuf[SDO_INFO_OFF + SDO_INFO_HEAD_SIZE + 3];
        OD_Entry_t *pEntry = OD_Find(reqIndex, reqSub);
        (void)valueInfo;

        if (pEntry == NULL)
        {
            SDO_InfoSendError(SDO_ABORT_NOT_EXIST);
            return;
        }

        uint16_t dataLen = COE_SIZE + SDO_INFO_HEAD_SIZE + 10U;

        MBX_PutU16(txBuf, MBX_OFF_LENGTH, dataLen);
        MBX_PutU16(txBuf, MBX_OFF_ADDRESS, g_rx_mbx_address);
        txBuf[MBX_OFF_CHANNEL] = g_rx_mbx_channel;
        txBuf[MBX_OFF_TYPE] = MBX_MakeType(MBX_TYPE_COE);

        MBX_PutU16(txBuf, COE_OFF, CoE_MakeResponseHeader(COE_SERVICE_SDO_INFO));

        SDO_InfoWriteHeader(txBuf, SDO_INFO_OPCODE_ENTRY_RESP, 0);

        uint16_t offset = SDO_INFO_OFF + SDO_INFO_HEAD_SIZE;
        MBX_PutU16(txBuf, offset + 0, reqIndex);
        MBX_PutU16(txBuf, offset + 2, (uint16_t)reqSub);
        MBX_PutU16(txBuf, offset + 4, pEntry->dataType);
        MBX_PutU16(txBuf, offset + 6, OD_GetBitLength(reqIndex, reqSub));
        MBX_PutU16(txBuf, offset + 8, OD_GetAccessFlags(reqIndex, reqSub));

        uint16_t timeout = 1000;
        while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }
        if (timeout == 0) {
            g_dbg_txTimeout++;
            return;
        }

        ESC_Mbx_Write(txBuf, MBX_HDR_SIZE + dataLen);
    }
    else
    {
        SDO_InfoSendError(SDO_ABORT_UNSUPPORTED);
    }
}

/* ================================================================
 * §4  公开接口
 * ================================================================ */

void CoE_Init(void)
{
    /* 对象字典静态定义, 初始化时只需复位运行态和诊断计数器。 */
    g_dbg_coe_rxCnt = 0;
    g_dbg_coe_procCnt = 0;
    g_dbg_txTimeout = 0;
    g_dbg_lastSvc = 0;
    g_dbg_lastCmd = 0;
    g_dbg_sdoInfoOp = 0;
    g_dbg_reqIndex = 0;
    g_dbg_reqSubIndex = 0;
    g_dbg_respCmd = 0;
    g_dbg_respIndex = 0;
    g_dbg_respSubIndex = 0;
    g_dbg_lastTxLen = 0;
    g_dbg_txMbxLen = 0;
    g_tx_mbx_counter = 0;
    g_sdo_seg_data = NULL;
    g_sdo_seg_len = 0;
    g_sdo_seg_off = 0;
    g_sdo_seg_index = 0;
    g_sdo_seg_subindex = 0;

}

uint8_t CoE_MainTask(void)
{
    uint8_t rxBuf[128];

    /* 检查邮箱是否有新消息 */
    if (!ESC_Mbx_RxFull())
    {
        return 0;
    }

    /* 读取邮箱 */
    if (ESC_Mbx_Read(rxBuf, sizeof(rxBuf)) != HAL_OK)
    {
        return 0;
    }

    g_dbg_coe_rxCnt++;  /* 调试: 收到邮箱数据 */

    /* 解析邮箱头 */
    MBX_Header_t *pMbxHdr = (MBX_Header_t *)rxBuf;
    g_rx_mbx_address = pMbxHdr->address;
    g_rx_mbx_channel = pMbxHdr->channel;

    /* 只处理 CoE 类型 (bits[3:0], mask 掉计数器 bits[7:4]) */
    if ((pMbxHdr->typeCounter & 0x0F) != MBX_TYPE_COE)
    {
        return 0;
    }

    /* 解析 CoE 头 */
    g_rx_coe_header_low = (uint16_t)(MBX_GetU16(rxBuf, COE_OFF) & 0x0FFFU);
    uint8_t coeService = CoE_GetService(rxBuf);
    g_dbg_lastSvc = coeService;

    /* 处理 SDO Info 请求 */
    if (coeService == COE_SERVICE_SDO_INFO)
    {
        g_dbg_lastCmd = 0;

        SDO_HandleInfo(rxBuf);
        g_dbg_coe_procCnt++;
        return 1;
    }

    /* 只处理 SDO 请求 */
    if (coeService != COE_SERVICE_SDO_REQUEST)
    {
        return 0;
    }

    /* 解析 SDO 头 */
    SDO_Header_t *pSDO = (SDO_Header_t *)(rxBuf + SDO_OFF);

    uint8_t  cmd      = pSDO->command;
    uint16_t index    = pSDO->index;
    uint8_t  subindex = pSDO->subindex;

    g_dbg_reqIndex = index;  /* 记录收到的Index，无论是否处理 */
    g_dbg_reqSubIndex = subindex;
    g_dbg_lastCmd = cmd;

    /* 根据命令分发 */
    if (cmd == SDO_CMD_UPLOAD_REQ)
    {
        /* Upload 请求: 主站读对象 */
        SDO_HandleUpload(index, subindex);
    }
    else if ((cmd & 0xE0U) == SDO_CMD_UPLOAD_SEG_REQ)
    {
        SDO_HandleUploadSegment(cmd);
    }
    else if ((cmd & 0xE0) == 0x20)  /* Download 请求: 主站写对象 */
    {
        SDO_HandleDownload(pSDO);
    }
    else
    {
        /* 不支持的命令 */
        SDO_SendAbort(index, subindex, SDO_ABORT_CMD_INVALID);
    }

    g_dbg_coe_procCnt++;  /* 调试: 成功处理了一次 SDO 请求 */
    return 1;  /* 处理了一次请求 */
}

uint16_t CoE_GetODSize(void)
{
    return OD_SIZE;
}
