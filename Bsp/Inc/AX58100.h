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
 * SM 状态寄存器 (SM_OFF_STATUS) 位定义:
 *   Bit [3]:  Mailbox/Buffer full —— 缓冲区有数据
 *               邮箱模式 SM0(M→S): 1 = 主站写入了新命令, 待 PDI 读取
 *               邮箱模式 SM1(S→M): 1 = PDI 写入的响应主站尚未取走
 */
#define SM_STATUS_MBX_FULL      (1U << 3) /* 邮箱满 / 缓冲区有数据         */

/*
 * SM 控制寄存器 (SM_OFF_CONTROL) 位定义 (AX58100 数据手册):
 *
 *   Bit [1:0]:  模式 (Operation Mode)
 *                 00 = 缓冲模式 (3-buffer)
 *                 01 = 保留
 *                 10 = 邮箱模式 (Single buffer)
 *                 11 = 保留
 *   Bit [3:2]:  方向 (Direction)
 *                 00 = ECAT 读, PDI 写  (S→M)
 *                 01 = ECAT 写, PDI 读  (M→S)
 *                 10 = 保留
 *                 11 = 保留
 *   Bit [4]:    ECAT 中断使能
 *   Bit [5]:    PDI 中断使能
 *   Bit [6]:    看门狗触发使能
 *   Bit [7]:    保留, 写 0
 */

/* ── 方向 (bit[3:2]) ── */
#define SM_CTRL_DIR_PDI2ECAT    0x00U   /* ECAT 读, PDI 写 (S→M)            */
#define SM_CTRL_DIR_ECAT2PDI    0x04U   /* ECAT 写, PDI 读 (M→S)            */

/* ── 模式 (bit[1:0]) ── */
#define SM_CTRL_MODE_BUFFERED   0x00U   /* 缓冲模式                          */
#define SM_CTRL_MODE_MAILBOX    0x02U   /* 邮箱模式 (Single buffer)          */

/* ── 中断 / 看门狗 (bit[6:4]) ── */
#define SM_CTRL_ECAT_IRQ        (1U << 4)
#define SM_CTRL_PDI_IRQ         (1U << 5)
#define SM_CTRL_WD_TRIGGER      (1U << 6)

/* ── 常用组合 (方向 | 模式 | 中断) ── */
#define SM_CTRL_M2S_MAILBOX     (SM_CTRL_MODE_MAILBOX | SM_CTRL_DIR_ECAT2PDI | SM_CTRL_PDI_IRQ)  /* 0x26 */
#define SM_CTRL_S2M_MAILBOX     (SM_CTRL_MODE_MAILBOX | SM_CTRL_DIR_PDI2ECAT | SM_CTRL_PDI_IRQ)  /* 0x22 */
#define SM_CTRL_M2S_BUFFERED    (SM_CTRL_MODE_BUFFERED | SM_CTRL_DIR_ECAT2PDI | SM_CTRL_PDI_IRQ | SM_CTRL_WD_TRIGGER) /* 0x64 */
#define SM_CTRL_S2M_BUFFERED    (SM_CTRL_MODE_BUFFERED | SM_CTRL_DIR_PDI2ECAT | SM_CTRL_PDI_IRQ)  /* 0x20 */

/* ── 向后兼容旧宏名 ── */
#define SM_DIR_WRITE            SM_CTRL_DIR_ECAT2PDI
#define SM_DIR_READ             SM_CTRL_DIR_PDI2ECAT
#define SM_MODE_BUFFERED        0x00U
#define SM_MODE_MAILBOX         0x02U

/* ── 2.9  看门狗 (0x0400~0x041F) ── */
#define ESC_REG_WDG_DIVIDER     0x0400U /* 看门狗分频器 (2B, 默认 0x03E8)  */
#define ESC_REG_WDG_PDI         0x0410U /* PDI 看门狗   (2B, 0=禁用)       */
#define ESC_REG_WDG_SM          0x0420U /* SM 看门狗    (2B, 0=禁用)       */
#define ESC_REG_WDG_EPU         0x0440U /* EPU 看门狗   (2B)               */

/* ── 2.9b EEPROM/SII 接口 (0x0500~0x050F) ── */
#define ESC_REG_EEPROM_CTRL     0x0500U /* EEPROM 控制/状态 (2B)          */
#define ESC_REG_EEPROM_ADDR     0x0502U /* EEPROM 地址       (2B)         */
#define ESC_REG_EEPROM_DATA     0x0504U /* EEPROM 数据       (4B)         */

/* ── 2.10 分布式时钟 DC (0x0900~0x09FF) ── */
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
void ESC_MbxSelfTest(void);        /* 邮箱自测: 写SM0→读状态→读回      */

/* ================================================================
 * §11 API: 看门狗 / 过程数据
 * ================================================================ */

void AX58100_WriteIdentity(void);        /* 写固定身份到 ESC (无 EEPROM)    */
void ESC_Watchdog_Config(void);         /* 禁用 PDI/SM 看门狗             */

/**
 * @brief  读主站输出数据 (SM2: M→S, MCU 读)
 * @param  pBuf: 输出缓冲区
 * @param  len:  读取字节数
 */
void ESC_ReadOutputData(uint8_t *pBuf, uint16_t len);

/**
 * @brief  写输入数据给主站 (SM3: S→M, MCU 写)
 * @param  pBuf: 要写入的数据
 * @param  len:  写入字节数
 */
void ESC_WriteInputData(uint8_t *pBuf, uint16_t len);

/* ================================================================
 * §12 API: 邮箱底层 (CoE 通信用, SM0/SM1)
 *
 *     SM0: 邮箱输出 (主→从) 0x1000  —— 主站写命令, MCU 读
 *     SM1: 邮箱输入 (从→主) 0x1080  —— MCU 写响应, 主站读
 *
 *     语义: 读/写必须访问到 SM 区尾字节地址, ESC 才会翻转 full 标志.
 * ================================================================ */

/**
 * @brief  查询 SM0 邮箱是否有主站发来的新命令
 * @retval 1 = 有 (SM0 状态 bit3=1), 0 = 无
 */
uint8_t ESC_Mbx_RxFull(void);

/**
 * @brief  查询 SM1 邮箱响应是否仍未被主站取走
 * @retval 1 = 满 (上次响应主站还没读), 0 = 空 (可写新响应)
 */
uint8_t ESC_Mbx_TxFull(void);

/**
 * @brief  从 SM0 读取主站邮箱命令
 * @param  pBuf: 输出缓冲区
 * @param  len:  缓冲区大小 (实际读 min(len, SM0 长度))
 * @retval HAL_OK / HAL_ERROR
 * @note   读到 SM0 区尾, ESC 自动清 full 标志, 准备接收下一条命令
 */
HAL_StatusTypeDef ESC_Mbx_Read(uint8_t *pBuf, uint16_t len);

/**
 * @brief  向 SM1 写入邮箱响应
 * @param  pBuf: 要写入的响应数据
 * @param  len:  数据长度 (写 min(len, SM1 长度))
 * @retval HAL_OK / HAL_ERROR
 * @note   写到 SM1 区尾, ESC 置 full 标志, 通知主站取走
 */
HAL_StatusTypeDef ESC_Mbx_Write(uint8_t *pBuf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __AX58100_H__ */
