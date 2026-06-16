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

/* 调试变量 — Watch 窗口观察 CoE 通信状态 */
volatile uint8_t g_dbg_coe_rxCnt   = 0;  /* CoE_MainTask 收到邮箱数据的次数   */
volatile uint8_t g_dbg_coe_procCnt = 0;  /* 成功处理 SDO 请求的次数          */

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
static uint32_t g_revisionNumber   = 0x00010000UL;  /* 子索引 3: 版本号         */
static uint32_t g_serialNumber     = 0x00000001UL;  /* 子索引 4: 序列号         */

/* ── 测试对象 (应用层可读写) ── */
static uint32_t g_testCounter      = 0;             /* 0x2000: 测试计数器 (rw)  */
static uint32_t g_testStatus       = 0x12345678UL;  /* 0x2001: 测试状态 (ro)    */

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

    /* 0x2000: 测试计数器 (读写) */
    { 0x2000, 0, OD_TYPE_UINT32, OD_ACCESS_RW, 0, 4, &g_testCounter },

    /* 0x2001: 测试状态 (只读) */
    { 0x2001, 0, OD_TYPE_UINT32, OD_ACCESS_RO, 0, 4, &g_testStatus },
};

#define OD_SIZE  (sizeof(g_objectDict) / sizeof(g_objectDict[0]))

/* ================================================================
 * §3  内部辅助函数
 * ================================================================ */

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

/**
 * @brief  发送 SDO Abort 响应
 * @param  index:     对象索引
 * @param  subindex:  子索引
 * @param  abortCode: SDO 错误码
 */
static void SDO_SendAbort(uint16_t index, uint8_t subindex, uint32_t abortCode)
{
    uint8_t txBuf[128];
    MBX_Header_t *pMbxHdr = (MBX_Header_t *)txBuf;
    CoE_Header_t *pCoEHdr = (CoE_Header_t *)(txBuf + sizeof(MBX_Header_t));
    SDO_Header_t *pSDO    = (SDO_Header_t *)(txBuf + sizeof(MBX_Header_t) + sizeof(CoE_Header_t));

    uint16_t dataLen = sizeof(CoE_Header_t) + sizeof(SDO_Header_t);

    /* 填充邮箱头 */
    pMbxHdr->length  = dataLen;
    pMbxHdr->address = 0;
    pMbxHdr->channel = 0;
    pMbxHdr->type    = MBX_TYPE_COE;

    /* 填充 CoE 头 */
    pCoEHdr->number   = 0;
    pCoEHdr->reserved = 0;
    pCoEHdr->service  = COE_SERVICE_SDO_RESPONSE;

    /* 填充 SDO Abort */
    pSDO->command  = SDO_CMD_ABORT;
    pSDO->index    = index;
    pSDO->subindex = subindex;
    pSDO->data     = abortCode;

    /* 等待上次响应被取走 */
    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    /* 发送 */
    ESC_Mbx_Write(txBuf, sizeof(MBX_Header_t) + dataLen);
}

/**
 * @brief  处理 SDO Upload 请求 (主站读对象)
 * @param  index:    对象索引
 * @param  subindex: 子索引
 */
static void SDO_HandleUpload(uint16_t index, uint8_t subindex)
{
    OD_Entry_t *pEntry = OD_Find(index, subindex);

    /* 对象不存在 */
    if (pEntry == NULL)
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_NOT_EXIST);
        return;
    }

    /* 检查访问权限 (只写对象不能读) */
    if (pEntry->access == OD_ACCESS_WO)
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_WRITEONLY);
        return;
    }

    uint8_t txBuf[128];
    MBX_Header_t *pMbxHdr = (MBX_Header_t *)txBuf;
    CoE_Header_t *pCoEHdr = (CoE_Header_t *)(txBuf + sizeof(MBX_Header_t));
    SDO_Header_t *pSDO    = (SDO_Header_t *)(txBuf + sizeof(MBX_Header_t) + sizeof(CoE_Header_t));

    uint16_t dataLen;

    /* 加速传输 (≤4 字节, 数据直接放在 SDO 头的 data 域) */
    if (pEntry->dataLen <= 4)
    {
        dataLen = sizeof(CoE_Header_t) + sizeof(SDO_Header_t);

        /* 填充邮箱头 */
        pMbxHdr->length  = dataLen;
        pMbxHdr->address = 0;
        pMbxHdr->channel = 0;
        pMbxHdr->type    = MBX_TYPE_COE;

        /* 填充 CoE 头 */
        pCoEHdr->number   = 0;
        pCoEHdr->reserved = 0;
        pCoEHdr->service  = COE_SERVICE_SDO_RESPONSE;

        /* 填充 SDO Upload 响应 (加速) */
        pSDO->command  = SDO_CMD_UPLOAD_EXPEDITED | ((4 - pEntry->dataLen) << 2);
        pSDO->index    = index;
        pSDO->subindex = subindex;

        /* 小端序拷贝数据到 data 域 */
        pSDO->data = 0;
        memcpy((void *)&pSDO->data, pEntry->pData, pEntry->dataLen);
    }
    /* 普通传输 (>4 字节, 数据跟在 SDO 头后面) */
    else
    {
        /* 缓冲区溢出检查 */
        if (pEntry->dataLen > (sizeof(txBuf) - sizeof(MBX_Header_t) - sizeof(CoE_Header_t) - sizeof(SDO_Header_t)))
        {
            SDO_SendAbort(index, subindex, SDO_ABORT_INTERNAL);
            return;
        }

        dataLen = sizeof(CoE_Header_t) + sizeof(SDO_Header_t) + pEntry->dataLen;

        /* 填充邮箱头 */
        pMbxHdr->length  = dataLen;
        pMbxHdr->address = 0;
        pMbxHdr->channel = 0;
        pMbxHdr->type    = MBX_TYPE_COE;

        /* 填充 CoE 头 */
        pCoEHdr->number   = 0;
        pCoEHdr->reserved = 0;
        pCoEHdr->service  = COE_SERVICE_SDO_RESPONSE;

        /* 填充 SDO Upload 响应 (普通) */
        pSDO->command  = SDO_CMD_UPLOAD_NORMAL;  /* e=0, s=1 → 指示大小有效 */
        pSDO->index    = index;
        pSDO->subindex = subindex;
        pSDO->data     = pEntry->dataLen;  /* data 域存总字节数 */

        /* 数据跟在 SDO 头后 */
        memcpy(txBuf + sizeof(MBX_Header_t) + sizeof(CoE_Header_t) + sizeof(SDO_Header_t),
               pEntry->pData, pEntry->dataLen);
    }

    /* 等待上次响应被取走 */
    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    /* 发送 */
    ESC_Mbx_Write(txBuf, sizeof(MBX_Header_t) + dataLen);
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

    /* 对象不存在 */
    if (pEntry == NULL)
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_NOT_EXIST);
        return;
    }

    /* 检查访问权限 (只读对象不能写) */
    if (pEntry->access == OD_ACCESS_RO)
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_READONLY);
        return;
    }

    /* 加速下载 (数据在 SDO 头的 data 域) */
    uint8_t cmd = pSDO->command;
    if (cmd & 0x02)  /* Expedited transfer */
    {
        uint8_t dataLen = 4 - ((cmd >> 2) & 0x03);

        /* 长度检查 (写入长度不得超出对象容量) */
        if (dataLen > pEntry->dataLen)
        {
            SDO_SendAbort(index, subindex, SDO_ABORT_LEN_MISMATCH);
            return;
        }

        /* 写数据 */
        memcpy(pEntry->pData, (const void *)&pSDO->data, dataLen);
    }
    /* 普通下载 (暂不支持, 需要分段传输) */
    else
    {
        SDO_SendAbort(index, subindex, SDO_ABORT_UNSUPPORTED);
        return;
    }

    /* 发送确认 */
    uint8_t txBuf[128];
    MBX_Header_t *pMbxHdr = (MBX_Header_t *)txBuf;
    CoE_Header_t *pCoEHdr = (CoE_Header_t *)(txBuf + sizeof(MBX_Header_t));
    SDO_Header_t *pSDOResp = (SDO_Header_t *)(txBuf + sizeof(MBX_Header_t) + sizeof(CoE_Header_t));

    uint16_t dataLen = sizeof(CoE_Header_t) + sizeof(SDO_Header_t);

    /* 填充邮箱头 */
    pMbxHdr->length  = dataLen;
    pMbxHdr->address = 0;
    pMbxHdr->channel = 0;
    pMbxHdr->type    = MBX_TYPE_COE;

    /* 填充 CoE 头 */
    pCoEHdr->number   = 0;
    pCoEHdr->reserved = 0;
    pCoEHdr->service  = COE_SERVICE_SDO_RESPONSE;

    /* 填充 SDO Download 响应 */
    pSDOResp->command  = SDO_CMD_DOWNLOAD_RESP;
    pSDOResp->index    = index;
    pSDOResp->subindex = subindex;
    pSDOResp->data     = 0;

    /* 等待上次响应被取走 */
    uint16_t timeout = 1000;
    while (ESC_Mbx_TxFull() && timeout--) { HAL_Delay(1); }

    /* 发送 */
    ESC_Mbx_Write(txBuf, sizeof(MBX_Header_t) + dataLen);
}

/* ================================================================
 * §4  公开接口
 * ================================================================ */

void CoE_Init(void)
{
    /* 目前对象字典是静态初始化, 无需额外操作 */
    /* 未来可在此处注册回调函数或初始化动态对象 */
}

uint8_t CoE_MainTask(void)
{
    uint8_t rxBuf[128];

    /* 检查邮箱是否有新消息 */
    if (!ESC_Mbx_RxFull())
    {
        return 0;  /* 无事件 */
    }

    /* 读取邮箱 */
    if (ESC_Mbx_Read(rxBuf, sizeof(rxBuf)) != HAL_OK)
    {
        return 0;
    }

    g_dbg_coe_rxCnt++;  /* 调试: 收到邮箱数据 */

    /* 解析邮箱头 */
    MBX_Header_t *pMbxHdr = (MBX_Header_t *)rxBuf;

    /* 只处理 CoE 类型 */
    if (pMbxHdr->type != MBX_TYPE_COE)
    {
        return 0;
    }

    /* 解析 CoE 头 */
    CoE_Header_t *pCoEHdr = (CoE_Header_t *)(rxBuf + sizeof(MBX_Header_t));

    /* 只处理 SDO 请求 */
    if (pCoEHdr->service != COE_SERVICE_SDO_REQUEST)
    {
        return 0;
    }

    /* 解析 SDO 头 */
    SDO_Header_t *pSDO = (SDO_Header_t *)(rxBuf + sizeof(MBX_Header_t) + sizeof(CoE_Header_t));

    uint8_t  cmd      = pSDO->command;
    uint16_t index    = pSDO->index;
    uint8_t  subindex = pSDO->subindex;

    /* 根据命令分发 */
    if (cmd == SDO_CMD_UPLOAD_REQ)
    {
        /* Upload 请求: 主站读对象 */
        SDO_HandleUpload(index, subindex);
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
