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
  *  ╔══════════════════════════════════════════════════════════════╗
  *  ║  文件结构                                                     ║
  *  ║    §1  PDI 命令码                                            ║
  *  ║    §2  ESC 寄存器映射                                        ║
  *  ║    §3  SyncManager 默认布局 (SM0~SM3)                        ║
  *  ║    §4  EtherCAT 状态机编码                                   ║
  *  ║    §5  数据结构                                              ║
  *  ║    §6  全局变量                                              ║
  *  ║    §7  API: PDI 传输层                                       ║
  *  ║    §8  API: SyncManager 管理                                 ║
  *  ║    §9  API: ESC 完整信息                                     ║
  *  ║    §10 API: 测试/诊断                                        ║
  *  ╚══════════════════════════════════════════════════════════════╝
  *
  *  开发进度:
  *    ✅ 第2步: ESC 单寄存器/块读写
  *    ✅ 第3步: ESC 完整信息读取 (AX58100_ReadESCInfo)
  *    ✅ 第4步: 状态机
  *    ✅ 第5步: SyncManager 配置
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
 * §1  ESC PDI 命令码
 *     SPI 2 字节地址模式下, 命令编码在 Byte1 低 3 位
 *     Byte0 = A[12:5],  Byte1 = {A[4:0], CMD[2:0]}
 * ================================================================ */
#define ESC_CMD_READ            0x03U   /* Read + Wait State (单字节读)   */
#define ESC_CMD_READ_NOWAIT     0x02U   /* Read No Wait    (块读)        */
#define ESC_CMD_WRITE           0x04U   /* Write                         */
#define ESC_MAX_BLOCK_SIZE      128U    /* 块读写最大字节数               */

/* ================================================================
 * §2  ESC 寄存器映射 (Beckhoff ETG.1000 标准, AX58100 兼容)
 *     地址空间: 0x0000 ~ 0x1FFF (8KB)
 *     其中 0x0000~0x0FFF 为寄存器区, 0x1000~0x1FFF 为过程数据 RAM
 * ================================================================ */

/* ── 2.1  身份信息 (0x0000~0x000F) ── */
#define ESC_REG_TYPE            0x0000U /* ESC 类型 (AX58100 = 0xC8)      */
#define ESC_REG_REVISION        0x0001U /* ESC 硬件版本                   */
#define ESC_REG_FMMU_SUPPORTED  0x0004U /* 支持的 FMMU 通道数             */
#define ESC_REG_SM_SUPPORTED    0x0005U /* 支持的 SyncManager 通道数      */
#define ESC_REG_RAM_SIZE        0x0006U /* 过程数据 RAM 大小 (KB)         */
#define ESC_REG_PORT_DESC       0x0007U /* 物理端口描述                   */
#define ESC_REG_FEATURES        0x0008U /* ESC 特性寄存器 (2 字节)        */

/* ── 2.2  MAC 地址 (0x0010~0x0017) ── */
#define ESC_REG_STATION_MAC     0x0010U /* 6 字节 MAC + 2 字节保留        */

/* ── 2.3  站别名 (0x0100~0x011F) ── */
#define ESC_REG_STATION_ALIAS   0x0100U /* 站别名 (2 字节)                */

/* ── 2.4  EtherCAT 状态机 (0x0120~0x013F) ── */
#define ESC_REG_AL_CONTROL      0x0120U /* AL Control  (主站→从站, 2B)   */
#define ESC_REG_AL_STATUS       0x0130U /* AL Status   (从站→主站, 2B)   */
#define ESC_REG_AL_STATUS_CODE  0x0134U /* AL Status Code        (2B)     */

/* ── 2.5  PDI 配置 (0x0140~0x015F, 上电时从 EEPROM 加载) ── */
#define ESC_REG_PDI_CONTROL     0x0140U /* PDI 类型 (SPI=0x05, 并口=0x80) */
#define ESC_REG_PDI_CONFIG      0x0150U /* SPI 模式 / CS 极性             */

/* ── 2.6  中断 / 状态 / 看门狗 (0x0200~0x03FF) ── */
#define ESC_REG_AL_EVENT        0x0220U /* AL Event Request       (2B)    */
#define ESC_REG_AL_EVENT_MASK   0x0204U /* AL Event Mask          (2B)    */
#define ESC_REG_WDG_DIVIDER     0x0400U /* 看门狗分频器                   */
#define ESC_REG_PDI_ERR_CNT     0x030DU /* PDI 错误计数                   */
#define ESC_REG_PDI_ERR_CODE    0x030EU /* PDI 最后错误原因码             */

/* ── 2.7  FMMU (0x0600~0x06FF, 8 通道 × 16 字节) ── */
#define ESC_REG_FMMU_BASE       0x0600U /* FMMU0 起始                     */
#define ESC_REG_FMMU_STRIDE     0x0010U /* 每 FMMU 16 字节                */

/* ── 2.8  SyncManager (0x0800~0x087F, 8 通道 × 16 字节) ── */
#define ESC_REG_SM_BASE         0x0800U /* SM0 起始地址                   */
#define ESC_REG_SM_STRIDE       0x0010U /* 每通道 16 字节                 */

/*
 * SM 通道寄存器布局 (相对 SM_BASE + N*0x10):
 *   Offset   Size  Access  Description
 *   ──────   ────  ──────  ────────────────────────────
 *   0x00     2B    R/W     物理起始地址 (Little-Endian)
 *   0x02     2B    R/W     长度 (字节数)
 *   0x04     1B    R/W     控制寄存器 ── 见下方 bit 定义
 *   0x05     1B    R       状态寄存器
 *   0x06     1B    R/W     激活 (写 1 启用该 SM 通道)
 *   0x07     1B    R/W     PDI 控制
 *   0x08~0x0F 8B   —       保留
 */

/* SM 内部偏移 */
#define SM_OFF_PHYS_START       0x00U   /* 物理起始地址 (2B LE)           */
#define SM_OFF_LENGTH           0x02U   /* 长度          (2B LE)          */
#define SM_OFF_CONTROL          0x04U   /* 控制寄存器    (1B)             */
#define SM_OFF_STATUS           0x05U   /* 状态寄存器    (1B, 只读)       */
#define SM_OFF_ACTIVATE         0x06U   /* 激活          (1B, 1=启用)     */
#define SM_OFF_PDI_CTRL         0x07U   /* PDI 控制      (1B)             */

/*
 * SM 控制寄存器 (SM_OFF_CONTROL) 位定义:
 *
 *   Bit [0]:    方向 (Direction)
 *                 0 = 主站写 → PDI 读  (ECAT → MCU)
 *                 1 = PDI 写 → 主站读  (MCU  → ECAT)
 *   Bit [2:1]:  模式 (Mode)
 *                 00 = 缓冲模式   — 过程数据用 (SM2/SM3)
 *                 01 = 3 缓冲模式 — 较少使用
 *                 10 = 邮箱模式   — CoE 通信用 (SM0/SM1)
 *                 11 = 保留
 *   Bit [7:3]:  保留 (写 0)
 */

/* ── 方向 ── */
#define SM_CTRL_DIR_ECAT2PDI    0x00U   /* 主站写, PDI 读 (M→S)           */
#define SM_CTRL_DIR_PDI2ECAT    0x01U   /* PDI 写, 主站读 (S→M)           */

/* ── 模式 (已左移到 bit[2:1] 位置, 直接 | 方向即可) ── */
#define SM_CTRL_MODE_BUFFERED   (0x00U << 1)  /* 缓冲模式   (0x00)         */
#define SM_CTRL_MODE_3BUFFER    (0x01U << 1)  /* 3 缓冲模式 (0x02)         */
#define SM_CTRL_MODE_MAILBOX    (0x02U << 1)  /* 邮箱模式   (0x04)         */

/* ── 常用组合 (方向 | 模式) ── */
#define SM_CTRL_M2S_MAILBOX     (SM_CTRL_MODE_MAILBOX | SM_CTRL_DIR_ECAT2PDI)  /* 0x04 */
#define SM_CTRL_S2M_MAILBOX     (SM_CTRL_MODE_MAILBOX | SM_CTRL_DIR_PDI2ECAT)  /* 0x05 */
#define SM_CTRL_M2S_BUFFERED    (SM_CTRL_MODE_BUFFERED | SM_CTRL_DIR_ECAT2PDI) /* 0x00 */
#define SM_CTRL_S2M_BUFFERED    (SM_CTRL_MODE_BUFFERED | SM_CTRL_DIR_PDI2ECAT) /* 0x01 */

/* ── 向后兼容旧宏名 ── */
#define SM_DIR_WRITE            SM_CTRL_DIR_ECAT2PDI
#define SM_DIR_READ             SM_CTRL_DIR_PDI2ECAT
#define SM_MODE_BUFFERED        0x00U
#define SM_MODE_MAILBOX         0x02U

/* ── 2.9  分布式时钟 DC (0x0900~0x09FF) ── */
#define ESC_REG_DC_BASE         0x0900U /* DC 配置起始                    */

/* ── 2.10 过程数据 RAM (0x1000~0x1FFF, AX58100 实际 9KB) ── */
#define ESC_RAM_BASE            0x1000U /* 过程数据 RAM 起始              */
#define ESC_RAM_SIZE_AX58100    (9 * 1024) /* AX58100 实际 9KB           */

/* ================================================================
 * §3  SyncManager 默认布局 (SM0~SM3)
 *     无主站自测时使用; 上线后由主站通过网线覆写
 *
 *     RAM 用量: 128 + 128 + 32 + 32 = 320 字节
 *     占用区间: 0x1000 ~ 0x113F (远小于 9KB 总容量)
 * ================================================================ */

#define SM0_DEFAULT_ADDR        0x1000U  /* 邮箱输出 (主→从)              */
#define SM0_DEFAULT_LEN         128U     /* 128 字节                      */
#define SM0_DEFAULT_CTRL        SM_CTRL_M2S_MAILBOX

#define SM1_DEFAULT_ADDR        0x1080U  /* 邮箱输入 (从→主)              */
#define SM1_DEFAULT_LEN         128U     /* 128 字节                      */
#define SM1_DEFAULT_CTRL        SM_CTRL_S2M_MAILBOX

#define SM2_DEFAULT_ADDR        0x1100U  /* 过程数据输出 (主→从)          */
#define SM2_DEFAULT_LEN         32U      /* 32 字节                       */
#define SM2_DEFAULT_CTRL        SM_CTRL_M2S_BUFFERED

#define SM3_DEFAULT_ADDR        0x1120U  /* 过程数据输入 (从→主)          */
#define SM3_DEFAULT_LEN         32U      /* 32 字节                       */
#define SM3_DEFAULT_CTRL        SM_CTRL_S2M_BUFFERED

/* ================================================================
 * §4  EtherCAT 状态机编码
 * ================================================================ */
#define ESC_STATE_INIT          0x01U   /* Init                          */
#define ESC_STATE_PREOP         0x02U   /* Pre-Operational               */
#define ESC_STATE_BOOT          0x03U   /* Bootstrap                     */
#define ESC_STATE_SAFEOP        0x04U   /* Safe-Operational              */
#define ESC_STATE_OP            0x08U   /* Operational                   */

/* AL Control 寄存器 bit 定义 */
#define AL_CTRL_ACK             (1 << 4)  /* 状态切换应答                */
#define AL_CTRL_ERROR_ACK       (1 << 5)  /* 错误确认                    */

/* AL Status Code (0x0134) 常见值 */
#define AL_STATUS_NO_ERROR      0x0000U
#define AL_STATUS_BOOT_NOT_SUPP 0x0012U

/* ================================================================
 * §5  数据结构
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

/** @brief SyncManager 单通道配置
 *
 *  对应 ESC 寄存器 SMn (0x0800 + n*0x10) 的前 8 字节.
 *  status 字段为只读, 仅在 ESC_SM_ReadConfig 时回填.
 */
typedef struct {
    uint16_t startAddr;         /* 物理起始地址 (2B, Little-Endian)     */
    uint16_t length;            /* 长度          (2B, Little-Endian)     */
    uint8_t  control;           /* 控制寄存器: 方向[0] + 模式[2:1]      */
    uint8_t  status;            /* 状态寄存器 (只读)                    */
    uint8_t  activate;          /* 激活 (1=启用该通道)                  */
    uint8_t  pdiCtrl;           /* PDI 控制                             */
} ESC_SM_Config_t;

/* ================================================================
 * §6  全局变量 — 调试用, Watch 窗口直接观察
 * ================================================================ */
extern volatile uint8_t g_escType;      /* 最近一次读的 ESC 类型          */
extern volatile uint8_t g_escVer;       /* 最近一次读的 ESC 版本          */
extern volatile uint8_t g_escError;     /* 读写测试错误计数 (0 = 通过)    */
extern AX58100_Info_t    g_escInfo;     /* ESC 完整信息                   */

/* ================================================================
 * §7  API: PDI 传输层  — ESC 寄存器读写
 * ================================================================ */

/* ── 单寄存器访问 ── */
HAL_StatusTypeDef ESC_ReadRegister(uint16_t addr, uint8_t *pData);
HAL_StatusTypeDef ESC_WriteRegister(uint16_t addr, uint8_t data);

/* ── 块访问 (连续寄存器, 地址自动递增) ── */
HAL_StatusTypeDef ESC_ReadBlock(uint16_t addr, uint8_t *pData, uint16_t size);
HAL_StatusTypeDef ESC_WriteBlock(uint16_t addr, uint8_t *pData, uint16_t size);

/* ── IRQ 状态 (每次 SPI 事务自动捕获到全局变量) ── */
void ESC_GetIRQStatus(uint8_t *irq0, uint8_t *irq1);

/* ================================================================
 * §8  API: SyncManager 管理
 *
 *     使用流程:
 *       自测:  ESC_SM_Init()              — 一键配好 SM0~SM3 默认值
 *       回读:  ESC_SM_ReadConfig(n, &cfg) — 读回寄存器实际值
 *       单配:  ESC_SM_Config(n, &cfg)     — 逐通道自定义
 *
 *     注意:  有主站时, SM 配置由主站在 PreOp 阶段通过网线写入,
 *            MCU 侧不需要调用这些函数.
 * ================================================================ */

/**
 * @brief  用默认布局初始化 SM0~SM3 (无主站自测用)
 * @note   一键写入 SM0/SM1/SM2/SM3 的起始地址/长度/控制/激活
 *         SM0: 邮箱 M→S,  0x1000, 128B
 *         SM1: 邮箱 S→M,  0x1080, 128B
 *         SM2: 缓冲 M→S,  0x1100,  32B
 *         SM3: 缓冲 S→M,  0x1120,  32B
 */
void ESC_SM_Init(void);

/**
 * @brief  读取单个 SM 通道的完整配置
 * @param  smIdx: SM 索引 (0~7)
 * @param  pCfg:  输出 — 填充通道配置 (含只读 status 字段)
 */
void ESC_SM_ReadConfig(uint8_t smIdx, ESC_SM_Config_t *pCfg);

/**
 * @brief  写入单个 SM 通道的配置
 * @param  smIdx: SM 索引 (0~7)
 * @param  pCfg:  要写入的配置 (activate 非 0 时激活通道)
 */
void ESC_SM_Config(uint8_t smIdx, const ESC_SM_Config_t *pCfg);

/* ── 向后兼容 ── */
void ESC_ReadSMConfig(uint8_t smIdx, uint16_t *pStartAddr, uint16_t *pLength,
                      uint8_t *pControl, uint8_t *pStatus);
void ESC_WriteSMConfig(uint8_t smIdx, uint16_t startAddr, uint16_t length,
                       uint8_t control, uint8_t activate);

/* ================================================================
 * §9  API: ESC 完整信息
 * ================================================================ */

/**
 * @brief  读 ESC 完整设备信息
 * @note   一次调用读齐所有身份/能力寄存器, 结果存入 g_escInfo
 *         仅需 5 次 SPI 事务 (2 次块读 + 3 次单读)
 * @retval HAL_OK / HAL_ERROR
 */
HAL_StatusTypeDef AX58100_ReadESCInfo(void);

/* ================================================================
 * §10 API: 测试 / 诊断
 * ================================================================ */

void ESC_TestReadID(void);         /* 读 ESC 类型/版本, 验证通信         */
void ESC_TestReadWrite(void);      /* 读写用户 RAM 区域, 验证 PDI 功能   */
void ESC_Diagnose(void);           /* 诊断: 一口气读关键寄存器            */

#ifdef __cplusplus
}
#endif

#endif /* __AX58100_H__ */
