/**
  ******************************************************************************
  * @file    AX58100.h
  * @brief   AX58100 EtherCAT Slave Controller (ESC) 驱动
  * @author  dongdong596
  * @date    2026-06-14
  *
  *  硬件:  AX58100 (ASIX) — 2 端口 EtherCAT 从站控制器
  *  接口:  SPI PDI (PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI, Mode 3)
  *  依赖:  spi.h (SPI 基础收发 + CS 控制)
  *
  *  开发进度:
  *    ✅ 第2步: ESC 单寄存器/块读写
  *    ✅ 第3步: ESC 完整信息读取 (AX58100_ReadESCInfo)
  *    ⬜ 第4步: 状态机
  *    ⬜ 第5步: SyncManager 配置
  *    ⬜ 第6步: CoE 邮箱协议
  *    ⬜ 第7步: 过程数据
  *    ⬜ 第8步: ESI/XML 从站描述文件
  ******************************************************************************
  */

#ifndef __AX58100_H__
#define __AX58100_H__

#include "main.h"
#include "spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ESC PDI 命令码 (Table 6-2: SPI commands CMD0 and CMD1)
 * ================================================================ */
#define ESC_CMD_READ            0x03U   /* Read + Wait State (单字节读)   */
#define ESC_CMD_READ_NOWAIT     0x02U   /* Read No Wait    (块读)        */
#define ESC_CMD_WRITE           0x04U   /* Write                         */
#define ESC_MAX_BLOCK_SIZE      128U    /* 块读写最大字节数               */

/* ================================================================
 * ESC 寄存器映射 (Beckhoff ESC 标准, AX58100 兼容)
 * ================================================================ */

/* --- 0x0000~0x000F: 身份信息 (Identity) --- */
#define ESC_REG_TYPE            0x0000U /* ESC 类型 (AX58100 = 0xC8)      */
#define ESC_REG_REVISION        0x0001U /* ESC 硬件版本                   */
#define ESC_REG_FMMU_SUPPORTED  0x0004U /* 支持的 FMMU 通道数             */
#define ESC_REG_SM_SUPPORTED    0x0005U /* 支持的 SyncManager 通道数      */
#define ESC_REG_RAM_SIZE        0x0006U /* 过程数据 RAM 大小 (KB)         */
#define ESC_REG_PORT_DESC       0x0007U /* 物理端口描述                   */
#define ESC_REG_FEATURES        0x0008U /* ESC 特性寄存器 (2 字节)        */

/* --- 0x0010~0x0017: MAC 地址 (Station Address) --- */
#define ESC_REG_STATION_MAC     0x0010U /* 6 字节 MAC + 2 字节保留        */

/* --- 0x0100~0x011F: 站别名 / 配置 --- */
#define ESC_REG_STATION_ALIAS   0x0100U /* 站别名 (2 字节)                */

/* --- 0x0120~0x013F: EtherCAT 状态机 --- */
#define ESC_REG_AL_CONTROL      0x0120U /* AL Control  (主站→从站, 2 字节) */
#define ESC_REG_AL_STATUS       0x0130U /* AL Status   (从站→主站, 2 字节) */
#define ESC_REG_AL_STATUS_CODE  0x0134U /* AL Status Code (2 字节)        */

/* --- 0x0140~0x015F: PDI 配置 (EEPROM 加载, 上电后只读) --- */
#define ESC_REG_PDI_CONTROL     0x0140U /* PDI 类型 (SPI=0x05, 并口=0x80) */
#define ESC_REG_PDI_CONFIG      0x0150U /* SPI 模式 / CS 极性             */

/* --- 0x0200~0x03FF: 中断 / 状态 / 看门狗 --- */
#define ESC_REG_AL_EVENT        0x0220U /* AL Event Request (2 字节)      */
#define ESC_REG_AL_EVENT_MASK   0x0204U /* AL Event Mask (2 字节)         */
#define ESC_REG_WDG_DIVIDER     0x0400U /* 看门狗分频器                   */
#define ESC_REG_PDI_ERR_CNT     0x030DU /* PDI 错误计数                   */
#define ESC_REG_PDI_ERR_CODE    0x030EU /* PDI 最后错误原因码             */

/* --- 0x0600~0x06FF: FMMU (8 通道 × 16 字节) --- */
#define ESC_REG_FMMU_BASE       0x0600U /* FMMU0 起始                     */
#define ESC_REG_FMMU_STRIDE     0x0010U /* 每 FMMU 16 字节                */

/* --- 0x0800~0x087F: SyncManager (8 通道 × 16 字节) --- */
#define ESC_REG_SM_BASE         0x0800U /* SM0 起始                       */
#define ESC_REG_SM_STRIDE       0x0010U /* 每 SM 16 字节                  */

/* SM 内部偏移 (加到 SM 基址上) */
#define SM_OFF_PHYS_START       0x00U   /* 物理起始地址 (2B, Little-Endian) */
#define SM_OFF_LENGTH           0x02U   /* 长度          (2B)              */
#define SM_OFF_CONTROL          0x04U   /* 控制寄存器    (1B)              */
#define SM_OFF_STATUS           0x05U   /* 状态寄存器    (1B, 只读)        */
#define SM_OFF_ACTIVATE         0x06U   /* 激活          (1B, 1=启用)      */
#define SM_OFF_PDI_CTRL         0x07U   /* PDI 控制      (1B)              */

/* SM 控制寄存器 (SM_OFF_CONTROL) bit 定义 */
#define SM_DIR_WRITE            0x00U   /* 主站写 → STM32 读               */
#define SM_DIR_READ             0x01U   /* STM32 写 → 主站读               */
#define SM_MODE_BUFFERED        0x00U   /* 缓冲模式 (过程数据 SM2/SM3)     */
#define SM_MODE_MAILBOX         0x02U   /* 邮箱模式 (CoE SM0/SM1)          */

/* ── SM 控制寄存器组合值 ── */
#define SM_CTRL_M2S_MAILBOX     ((SM_MODE_MAILBOX << 1) | SM_DIR_WRITE)   /* 0x04 */
#define SM_CTRL_S2M_MAILBOX     ((SM_MODE_MAILBOX << 1) | SM_DIR_READ)    /* 0x05 */
#define SM_CTRL_M2S_BUFFERED    ((SM_MODE_BUFFERED << 1) | SM_DIR_WRITE)  /* 0x00 */
#define SM_CTRL_S2M_BUFFERED    ((SM_MODE_BUFFERED << 1) | SM_DIR_READ)   /* 0x01 */

/* --- 0x0900~0x09FF: 分布式时钟 (Distributed Clock) --- */
#define ESC_REG_DC_BASE         0x0900U /* DC 配置起始                    */

/* --- 0x1000~0x1FFF: 过程数据 RAM (AX58100: 9KB) --- */
#define ESC_RAM_BASE            0x1000U /* 过程数据 RAM 起始              */
#define ESC_RAM_SIZE_AX58100    (9 * 1024) /* AX58100 实际 9KB             */

/* ================================================================
 * EtherCAT 状态机编码
 * ================================================================ */
#define ESC_STATE_INIT          0x01U   /* Init                          */
#define ESC_STATE_PREOP         0x02U   /* Pre-Operational               */
#define ESC_STATE_BOOT          0x03U   /* Bootstrap                     */
#define ESC_STATE_SAFEOP        0x04U   /* Safe-Operational              */
#define ESC_STATE_OP            0x08U   /* Operational                   */

/* AL Control 寄存器 bit 定义 */
#define AL_CTRL_ACK             (1 << 4)  /* 状态切换应答                  */
#define AL_CTRL_ERROR_ACK       (1 << 5)  /* 错误确认                      */

/* AL Status Code (0x0134) 常见值 */
#define AL_STATUS_NO_ERROR      0x0000U
#define AL_STATUS_BOOT_NOT_SUPP 0x0012U

/* ================================================================
 * 数据结构
 * ================================================================ */

/** @brief ESC 完整设备信息 */
typedef struct {
    uint8_t  type;              /* 0x0000: ESC 类型 (AX58100=0xC8)      */
    uint8_t  revision;          /* 0x0001: 硬件版本                     */
    uint8_t  fmmuSupported;     /* 0x0004: FMMU 通道数                  */
    uint8_t  smSupported;       /* 0x0005: SM 通道数                    */
    uint8_t  ramSizeKB;         /* 0x0006: RAM 大小 (KB)                */
    uint8_t  portDesc;          /* 0x0007: 端口描述                     */
    uint16_t features;          /* 0x0008: ESC 特性 (2 字节)            */
    uint8_t  mac[6];            /* 0x0010: MAC 地址                     */
    uint8_t  pdiControl;        /* 0x0140: PDI 接口类型                 */
    uint8_t  pdiErrorCnt;       /* 0x030D: PDI 错误计数                 */
    uint8_t  pdiErrorCode;      /* 0x030E: 最后 PDI 错误码              */
} AX58100_Info_t;

/* ================================================================
 * 全局变量 — 调试用, Watch 窗口直接观察
 * ================================================================ */
extern volatile uint8_t g_escType;      /* 最近一次读的 ESC 类型          */
extern volatile uint8_t g_escVer;       /* 最近一次读的 ESC 版本          */
extern volatile uint8_t g_escError;     /* 读写测试错误计数 (0 = 通过)    */
extern AX58100_Info_t    g_escInfo;     /* ESC 完整信息 (调用 ReadESCInfo 后有效) */

/* ================================================================
 * PDI 传输层 — ESC 寄存器读写
 * ================================================================ */

/* 单寄存器访问 */
HAL_StatusTypeDef ESC_ReadRegister(uint16_t addr, uint8_t *pData);
HAL_StatusTypeDef ESC_WriteRegister(uint16_t addr, uint8_t data);

/* 块访问 (连续寄存器, 地址自动递增) */
HAL_StatusTypeDef ESC_ReadBlock(uint16_t addr, uint8_t *pData, uint16_t size);
HAL_StatusTypeDef ESC_WriteBlock(uint16_t addr, uint8_t *pData, uint16_t size);

/* IRQ 状态 (每次 SPI 事务自动捕获) */
void ESC_GetIRQStatus(uint8_t *irq0, uint8_t *irq1);

/* SyncManager 配置 */
void ESC_ReadSMConfig(uint8_t smIdx, uint16_t *pStartAddr, uint16_t *pLength,
                      uint8_t *pControl, uint8_t *pStatus);
void ESC_WriteSMConfig(uint8_t smIdx, uint16_t startAddr, uint16_t length,
                       uint8_t control, uint8_t activate);

/* ────────────────────────────────────────────────────────────────
 * 第3步: ESC 信息读取
 * ──────────────────────────────────────────────────────────────── */

/** @brief 读 ESC 完整设备信息
 *  @note  一次调用读齐所有身份/能力寄存器, 结果存入 g_escInfo
 *  @retval HAL_OK / HAL_ERROR */
HAL_StatusTypeDef AX58100_ReadESCInfo(void);

/* ────────────────────────────────────────────────────────────────
 * 测试 / 诊断
 * ──────────────────────────────────────────────────────────────── */

void ESC_TestReadID(void);         /* 读 ESC 类型/版本, 验证通信         */
void ESC_TestReadWrite(void);      /* 读写用户 RAM 区域, 验证 PDI 功能   */
void ESC_Diagnose(void);           /* 诊断: 一口气读关键寄存器            */

#ifdef __cplusplus
}
#endif

#endif /* __AX58100_H__ */
