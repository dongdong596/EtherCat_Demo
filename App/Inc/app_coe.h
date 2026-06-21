/**
  ******************************************************************************
  * @file    app_coe.h
  * @brief   CoE (CAN over EtherCAT) 协议栈 — SDO 服务 + 对象字典
  * @author  dongdong596
  * @date    2026-06-17
  *
  *  CoE 协议层次:
  *    邮箱头 (6B) + CoE 头 (2B) + 数据
  *
 *  SDO 服务:
 *    Upload   — 主站读从站对象字典
 *    Download — 主站写从站对象字典
 *    Info     — TwinCAT CoE Online 扫描对象列表/对象描述/条目描述
  *
  *  强制对象 (ETG.1000.6):
  *    0x1000  Device Type         — 设备类型
  *    0x1008  Device Name         — 设备名称
  *    0x1009  Hardware Version    — 硬件版本
  *    0x100A  Software Version    — 软件版本
 *    0x1018  Identity Object     — 厂商ID/产品代码/序列号/版本
 *    0x1600/0x1A00               — RxPDO/TxPDO mapping
 *    0x1C00/0x1C12/0x1C13        — SM 类型与 PDO assign
  ******************************************************************************
  */

#ifndef __APP_COE_H__
#define __APP_COE_H__

#include "AX58100.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * §1  邮箱协议 (Mailbox Header, ETG.1000.4)
 * ================================================================ */

/** @brief 邮箱头 (6 字节, 小端序, ETG.1000.4) */
typedef struct __attribute__((packed)) {
    uint16_t length;        /* 数据长度 (不含邮箱头本身)            */
    uint16_t address;       /* 从站地址 (0=广播)                   */
    uint8_t  channel;       /* 通道 (bits[5:0]) + 优先级(bits[7:6]) */
    uint8_t  typeCounter;   /* bits[3:0]=类型, bits[7:4]=计数器     */
} MBX_Header_t;

/* 邮箱类型编码 */
#define MBX_TYPE_AOE        0x01U   /* ADS over EtherCAT */
#define MBX_TYPE_EOE        0x02U   /* Ethernet over EtherCAT */
#define MBX_TYPE_COE        0x03U   /* CAN over EtherCAT */
#define MBX_TYPE_FOE        0x04U   /* File over EtherCAT */
#define MBX_TYPE_SOE        0x05U   /* Servo over EtherCAT */
#define MBX_TYPE_VOE        0x0FU   /* Vendor over EtherCAT */

/* ================================================================
 * §2  CoE 协议 (ETG.1000.6)
 * ================================================================ */

/** @brief CoE 头 (2 字节) */
typedef struct __attribute__((packed)) {
    uint16_t number   : 9;  /* SDO 索引/号码               */
    uint16_t reserved : 3;  /* 保留                        */
    uint16_t service  : 4;  /* CoE 服务类型 (见下方定义)   */
} CoE_Header_t;

/* CoE 服务类型 (CoE_Header_t.service) */
#define COE_SERVICE_SDO_REQUEST     0x02U   /* SDO 请求 (主→从)  */
#define COE_SERVICE_SDO_RESPONSE    0x03U   /* SDO 响应 (从→主)  */
#define COE_SERVICE_TXPDO           0x04U   /* TxPDO (从→主)     */
#define COE_SERVICE_RXPDO           0x05U   /* RxPDO (主→从)     */
#define COE_SERVICE_TXPDO_REMOTE    0x06U   /* TxPDO 远程请求    */
#define COE_SERVICE_RXPDO_REMOTE    0x07U   /* RxPDO 远程请求    */
#define COE_SERVICE_SDO_INFO        0x08U   /* SDO 信息服务      */

/* ================================================================
 * §3  SDO 协议 (ETG.1000.6 §5.6)
 * ================================================================ */

/** @brief SDO 头 (8 字节, CANopen DS301) */
typedef struct __attribute__((packed)) {
    uint8_t  command;       /* SDO 命令码 (见下方定义)         */
    uint16_t index;         /* 对象索引 (0x1000~0xFFFF)       */
    uint8_t  subindex;      /* 子索引 (0x00~0xFF)             */
    uint32_t data;          /* 数据或总大小 (小端序)           */
} SDO_Header_t;

/* ── SDO 命令码 ── */

/* Upload 请求 (主站读对象字典) */
#define SDO_CMD_UPLOAD_REQ          0x40U   /* 初始上传请求              */
#define SDO_CMD_UPLOAD_SEG_REQ      0x60U   /* 分段上传请求              */

/* Upload 响应 (从站回复数据) */
#define SDO_CMD_UPLOAD_EXPEDITED    0x43U   /* 加速上传 (≤4 字节, 立即回) */
#define SDO_CMD_UPLOAD_NORMAL       0x41U   /* 普通上传 (>4 字节, 需分段) */
#define SDO_CMD_UPLOAD_SEG_RESP     0x00U   /* 分段上传响应              */

/* Download 请求 (主站写对象字典) */
#define SDO_CMD_DOWNLOAD_REQ        0x20U   /* 初始下载请求              */
#define SDO_CMD_DOWNLOAD_SEG_REQ    0x00U   /* 分段下载请求              */

/* Download 响应 (从站确认) */
#define SDO_CMD_DOWNLOAD_RESP       0x60U   /* 下载响应                  */
#define SDO_CMD_DOWNLOAD_SEG_RESP   0x20U   /* 分段下载响应              */

/* Abort (错误响应) */
#define SDO_CMD_ABORT               0x80U   /* SDO 中止 (错误码在 data 域) */

/* ── SDO Abort 错误码 ── */
#define SDO_ABORT_TOGGLE_BIT        0x05030000UL  /* Toggle bit 不匹配      */
#define SDO_ABORT_TIMEOUT           0x05040000UL  /* SDO 超时              */
#define SDO_ABORT_CMD_INVALID       0x05040001UL  /* 无效命令              */
#define SDO_ABORT_UNSUPPORTED       0x06010000UL  /* 不支持的访问          */
#define SDO_ABORT_WRITEONLY         0x06010001UL  /* 只写对象              */
#define SDO_ABORT_READONLY          0x06010002UL  /* 只读对象              */
#define SDO_ABORT_NOT_EXIST         0x06020000UL  /* 对象不存在            */
#define SDO_ABORT_NO_MAP            0x06040041UL  /* 对象不能映射到PDO     */
#define SDO_ABORT_PDO_LEN           0x06040042UL  /* PDO 长度超限          */
#define SDO_ABORT_PARAM_INCOMPAT    0x06040043UL  /* 参数不兼容            */
#define SDO_ABORT_INTERNAL          0x06040047UL  /* 内部不兼容            */
#define SDO_ABORT_HW_ERROR          0x06060000UL  /* 硬件错误              */
#define SDO_ABORT_LEN_MISMATCH      0x06070010UL  /* 长度不匹配            */
#define SDO_ABORT_LEN_TOO_HIGH      0x06070012UL  /* 长度过大              */
#define SDO_ABORT_LEN_TOO_LOW       0x06070013UL  /* 长度过小              */
#define SDO_ABORT_SUBINDEX_INVALID  0x06090011UL  /* 子索引不存在          */
#define SDO_ABORT_VALUE_RANGE       0x06090030UL  /* 值超出范围            */
#define SDO_ABORT_VALUE_TOO_HIGH    0x06090031UL  /* 值过大                */
#define SDO_ABORT_VALUE_TOO_LOW     0x06090032UL  /* 值过小                */
#define SDO_ABORT_MAX_LT_MIN        0x06090036UL  /* 最大值 < 最小值       */
#define SDO_ABORT_GENERAL           0x08000000UL  /* 通用错误              */
#define SDO_ABORT_DATA_STORE        0x08000020UL  /* 数据无法存储          */
#define SDO_ABORT_DATA_LOCAL        0x08000021UL  /* 本地控制错误          */
#define SDO_ABORT_DATA_STATE        0x08000022UL  /* 当前状态下无法访问    */
#define SDO_ABORT_NO_OD             0x08000023UL  /* 对象字典不存在        */

/* ── SDO Info OpCodes (ETG.1000.6 §5.6.2) ── */
#define SDO_INFO_OPCODE_LIST_REQ        0x01U  /* Get OD List Request       */
#define SDO_INFO_OPCODE_LIST_RESP       0x02U  /* Get OD List Response      */
#define SDO_INFO_OPCODE_OBJ_REQ         0x03U  /* Get Object Description Req*/
#define SDO_INFO_OPCODE_OBJ_RESP        0x04U  /* Get Object Description Rsp*/
#define SDO_INFO_OPCODE_ENTRY_REQ       0x05U  /* Get Entry Description Req */
#define SDO_INFO_OPCODE_ENTRY_RESP      0x06U  /* Get Entry Description Rsp */
#define SDO_INFO_OPCODE_ERROR           0x07U  /* SDO Info Error Response   */

/* SDO Info List Types */
#define SDO_INFO_LIST_TYPE_LENGTH       0x00U  /* All objects, length only  */
#define SDO_INFO_LIST_TYPE_ALL          0x01U  /* All objects, with data    */
#define SDO_INFO_LIST_TYPE_RXPDO        0x02U  /* RxPDO mappable objects    */
#define SDO_INFO_LIST_TYPE_TXPDO        0x03U  /* TxPDO mappable objects    */

/* ================================================================
 * §4  对象字典数据结构
 * ================================================================ */

/** @brief 对象字典条目 (单个对象或子对象) */
typedef struct {
    uint16_t index;         /* 对象索引 (0x1000~0xFFFF)           */
    uint8_t  subindex;      /* 子索引 (0x00~0xFF, 0=主对象)       */
    uint8_t  dataType;      /* 数据类型 (0x05=UINT32, 0x09=Vis String) */
    uint8_t  access;        /* 访问权限: 'r'=只读, 'w'=只写, 'rw'=读写 */
    uint8_t  reserved;      /* 字节对齐保留                       */
    uint16_t dataLen;       /* 数据长度 (字节)                    */
    void    *pData;         /* 指向实际数据的指针                 */
} OD_Entry_t;

/* 数据类型编码 (CANopen DS301) */
#define OD_TYPE_BOOLEAN     0x01U   /* BOOLEAN   (1 字节)  */
#define OD_TYPE_INT8        0x02U   /* INTEGER8  (1 字节)  */
#define OD_TYPE_INT16       0x03U   /* INTEGER16 (2 字节)  */
#define OD_TYPE_INT32       0x04U   /* INTEGER32 (4 字节)  */
#define OD_TYPE_UINT8       0x05U   /* UNSIGNED8 (1 字节)  */
#define OD_TYPE_UINT16      0x06U   /* UNSIGNED16(2 字节)  */
#define OD_TYPE_UINT32      0x07U   /* UNSIGNED32(4 字节)  */
#define OD_TYPE_REAL32      0x08U   /* REAL32    (4 字节)  */
#define OD_TYPE_VIS_STRING  0x09U   /* VISIBLE_STRING      */
#define OD_TYPE_OCTET_STR   0x0AU   /* OCTET_STRING        */
#define OD_TYPE_UNICODE_STR 0x0BU   /* UNICODE_STRING      */
#define OD_TYPE_DOMAIN      0x0FU   /* DOMAIN (任意长度)   */

/* 访问权限编码 */
#define OD_ACCESS_RO        'r'     /* 只读 */
#define OD_ACCESS_WO        'w'     /* 只写 */
#define OD_ACCESS_RW        'b'     /* 读写 (both) */

/* ================================================================
 * §5  调试变量 (Watch 窗口观察)
 * ================================================================ */

extern volatile uint8_t  g_dbg_coe_rxCnt;     /* 收到 CoE 邮箱帧次数 */
extern volatile uint8_t  g_dbg_coe_procCnt;   /* 已处理 SDO/SDO Info 请求次数 */
extern volatile uint8_t  g_dbg_txTimeout;     /* 等待 SM1 空闲超时次数 */
extern volatile uint8_t  g_dbg_lastSvc;       /* 最近一次 CoE service */
extern volatile uint8_t  g_dbg_lastCmd;       /* 最近一次 SDO command */
extern volatile uint8_t  g_dbg_sdoInfoOp;     /* 最近一次 SDO Info opcode */
extern volatile uint16_t g_dbg_reqIndex;      /* 最近一次 SDO 请求 index */
extern volatile uint8_t  g_dbg_reqSubIndex;   /* 最近一次 SDO 请求 subindex */
extern volatile uint8_t  g_dbg_respCmd;       /* 最近一次 SDO 响应 command */
extern volatile uint16_t g_dbg_respIndex;     /* 最近一次 SDO 响应 index */
extern volatile uint8_t  g_dbg_respSubIndex;  /* 最近一次 SDO 响应 subindex */
extern volatile uint16_t g_dbg_lastTxLen;     /* 最近一次发送总长度, 含 6B 邮箱头 */
extern volatile uint16_t g_dbg_txMbxLen;      /* 最近一次 Mailbox Length 字段 */
extern volatile uint32_t g_testCounter;       /* 0x2000: LED Output Mask */
extern volatile uint32_t g_testStatus;        /* 0x2001: Digital Input Mask */

/* ================================================================
 * §6  CoE 主任务 API
 * ================================================================ */

/**
 * @brief  CoE 协议栈初始化
 * @note   初始化对象字典和内部状态
 */
void CoE_Init(void);

/**
 * @brief  CoE 主任务 — 轮询邮箱, 处理 SDO 请求
 * @note   在 PREOP/SAFEOP/OP 状态下循环调用
 * @retval 0=无事件, 1=处理了一次请求
 */
uint8_t CoE_MainTask(void);

/**
 * @brief  获取对象字典条目数
 * @retval 对象字典总条目数
 */
uint16_t CoE_GetODSize(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_COE_H__ */
