# SPI 驱动修改日志

## 项目信息
- 工程：`EtherCat_Test` (STM32F103VB + AX58100 EtherCAT ESC)
- IDE：Keil MDK-ARM
- HAL 库：STM32F1xx HAL Driver

---

## 修改概览

| 文件 | 修改内容 |
|------|---------|
| `Core/Inc/spi.h` | 添加 CS 宏、命令码、ESC 函数声明 |
| `Core/Src/spi.c` | SPI Mode3、ESC PDI 协议、测试函数、bug 修复 |
| `Core/Src/main.c` | 测试入口改为 ESC 测试 |

---

## 一、SPI 硬件参数变更

| 参数 | 修改前 | 修改后 | 原因 |
|------|--------|--------|------|
| CLKPolarity | `SPI_POLARITY_LOW` | `SPI_POLARITY_HIGH` | AX58100 要求 Mode3 (CPOL=1) |
| CLKPhase | `SPI_PHASE_1EDGE` | `SPI_PHASE_2EDGE` | AX58100 要求 Mode3 (CPHA=1) |
| BaudRatePrescaler | `/32 (2.25MHz)` | `/16 (4.5MHz)` | AX58100 最大 20MHz |
| CS 控制 | 无 | PA4 软件控制 | SCS_ESC 片选 |

### SPI 当前配置

```
模式：   主模式 (Master), SPI Mode 3
SCK：    空闲高电平, 第二个边沿采样
时钟：   PCLK2/16 = 4.5 MHz
数据位： 8 bit, MSB 先发
NSS：    软件模式
引脚：   PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI
```

---

## 二、spi.h 新增内容

```c
/* CS 片选引脚定义 */
#define SPI_CS_PORT             GPIOA
#define SPI_CS_ESC_PIN          GPIO_PIN_4
#define SPI_CS_LOW()            HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_ESC_PIN, GPIO_PIN_RESET)
#define SPI_CS_HIGH()           HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_ESC_PIN, GPIO_PIN_SET)

/* ESC PDI 命令码 (Table 6-2) */
#define ESC_CMD_READ            0x03U   // 011: 读 + 等待状态
#define ESC_CMD_WRITE           0x04U   // 100: 写
```

### 新增函数声明

| 分类 | 函数 | 功能 |
|------|------|------|
| 基础收发 | `SPI_WriteByte` / `SPI_ReadByte` / `SPI_WriteReadByte` | 单字节收发 |
| 基础收发 | `SPI_WriteBuffer` / `SPI_ReadBuffer` / `SPI_WriteReadBuffer` | 多字节收发 |
| ESC 访问 | `ESC_ReadRegister(addr, &data)` | 读 ESC 寄存器 |
| ESC 访问 | `ESC_WriteRegister(addr, data)` | 写 ESC 寄存器 |
| ESC 访问 | `ESC_ReadBlock(addr, buf, size)` | 块读 (最大128字节) |
| ESC 访问 | `ESC_WriteBlock(addr, buf, size)` | 块写 (最大128字节) |
| ESC 访问 | `ESC_GetIRQStatus(&irq0, &irq1)` | 获取中断状态 |
| 测试 | `ESC_TestReadID()` | 读取 ESC 类型/版本 |
| 测试 | `ESC_TestReadWrite()` | 读写用户 RAM |

---

## 三、spi.c 新增/修改内容

### 3.1 初始化修改

```c
// MX_SPI1_Init() 末尾新增:
SPI_CS_HIGH();  // 确保 CS 初始为高 (不选中从设备)
```

### 3.2 地址编码函数（新增）

```c
static void ESC_EncodeAddress(uint16_t addr, uint8_t cmd,
                               uint8_t *pAddr0, uint8_t *pAddr1)
{
    *pAddr0 = (uint8_t)((addr >> 5) & 0xFF);             // A[12:5]
    *pAddr1 = (uint8_t)(((addr & 0x1F) << 3) | (cmd & 0x07)); // A[4:0]<<3 | CMD
}
```

### 3.3 ESC SPI PDI 通信协议

基于 AX58100 Datasheet Table 6-1/6-2 和 Beckhoff ESC SPI 标准：

**读寄存器 (CMD=0x03)**

```
CS LOW →
  MOSI: [A[12:5]] [A[4:0]<<3|0x03] [0xFF等待] [0xFF终止]
  MISO: [IRQ 0x0220] [IRQ 0x0221] [dummy   ] [数据    ]
→ CS HIGH  →  返回 MISO[3]
```

**写寄存器 (CMD=0x04)**

```
CS LOW →
  MOSI: [A[12:5]] [A[4:0]<<3|0x04] [数据]
  MISO: [IRQ 0x0220] [IRQ 0x0221] [xx  ]
→ CS HIGH
```

### 3.4 ESC 寄存器参考

| 地址 | 寄存器 | 说明 |
|------|--------|------|
| 0x0000 | ESC Type | 芯片类型 |
| 0x0001 | ESC Version | 芯片版本 |
| 0x0140 | PDI Control | PDI 类型选择 (SPI=0x05) |
| 0x0150 | PDI Configuration | SPI 模式 / CS 极性 |
| 0x0220 | AL Event Request [7:0] | 中断请求状态 |
| 0x0221 | AL Event Request [15:8] | 中断请求状态 |
| 0x1000+ | Process Data RAM | 用户数据区域 |

### 3.5 默认测试（ESC_TestReadID）

```c
void ESC_TestReadID(void)
{
    ESC_ReadRegister(0x0000, &escType);   // 读 Type
    ESC_ReadRegister(0x0001, &escVer);    // 读 Version
    ESC_GetIRQStatus(&irq0, &irq1);       // 读 IRQ 状态
}
```

---

## 四、Bug 修复清单

| # | 问题 | 修复 |
|---|------|------|
| 1 | **CS 上电初始为低** — PA4 默认 RESET，上电瞬间选中 AX58100，可能产生误触发 | `MX_SPI1_Init` 末尾加 `SPI_CS_HIGH()` |
| 2 | **SPI_ReadBuffer 缓冲区越界** — size>128 时只填充了 dummyTx[0..127]，但 `HAL_SPI_TransmitReceive` 读取到 size 字节 | 加门禁 `if(size>128) return HAL_ERROR` |
| 3 | **ESC 块读写栈溢出** — `txBuf[259]+rxBuf[259]≈518字节`，超 STM32F103 默认栈 | 改为 `static` 缓冲区，`ESC_MAX_BLOCK_SIZE` 降至 128 |

---

## 五、使用说明

### 5.1 编译下载
1. Keil MDK 打开 `MDK-ARM/EtherCat_Test.uvprojx`
2. 编译（F7），下载（F8）

### 5.2 测试选择
在 `main.c` 的 while 循环中切换：

```c
ESC_TestReadID();       // 测试1: 读 ESC ID (当前默认)
// ESC_TestReadWrite(); // 测试2: 读写 RAM
// SPI_LoopbackTest();  // 测试3: 自环回 (需短接 MOSI/MISO)
// SPI_SendTestPattern(); // 测试4: 波形测试 (需示波器)
```

### 5.3 调试方法
1. 进入 Keil 调试模式 (Ctrl+F5)
2. 在 `spi.c` 中目标测试函数的 `__NOP()` 行设断点
3. 全速运行 (F5)，在断点处观察变量值

### 5.4 前置条件
> ⚠️ **AX58100 的 EEPROM 必须已正确配置**：
> - PDI Control (0x0140) = **0x05** (SPI Slave)
> - PDI Configuration (0x0150) 位[1:0] = **11** (Mode 3)
> 
> 如果 EEPROM 未烧录或配置不正确，SPI 通信将无响应。
> 参考文件：`AX58100_ESI_DesignNote_v106.pdf`

---

## 六、SPI 逻辑分析仪期望波形

### 读 0x0000 寄存器（4 字节事务）

```
CS   ‾‾‾\_________________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
SCK  ‾‾‾‾‾\___/‾\___/‾\___/‾\___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
MOSI ______/0x00\__/0x03\__/0xFF\__/0xFF\___________
MISO ______/IRQ0 \__/IRQ1 \__/  xx \__/DATA\_________
         Byte0    Byte1    Wait    Read Term
         (地址)   (地址+命令) (等待) (读数据+终止)
```

### 写 0x0140 寄存器（3 字节事务）

```
CS   ‾‾‾\___________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
SCK  ‾‾‾‾‾\___/‾\___/‾\___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
MOSI ______/0x0A\__/0x04\__/0x05\_____________
MISO ______/IRQ0 \__/IRQ1 \__/  xx \___________
         Byte0    Byte1    Data
         (地址)   (地址+命令) (写数据)
```

---

## 七、参考资料

| 文件 | 说明 |
|------|------|
| `AX58100_Full_Datasheet_v103.pdf` | 完整数据手册（寄存器映射、SPI 协议、时序） |
| `AX58100_ESI_DesignNote_v106.pdf` | ESI/EEPROM 设计说明 |
| `AX58100_Datasheet_v103.pdf` | 基础数据手册 |

### 数据手册关键章节

| 章节 | 内容 |
|------|------|
| 6 SPI Slave | PDI SPI 协议、地址模式、命令定义 |
| Table 6-1 | 2/3 字节地址模式结构 |
| Table 6-2 | SPI 命令码 CMD0/CMD1 |
| Figure 14-17 | PDI SPI 读时序 (2字节地址, 1字节数据, 带等待) |
| Figure 14-19 | PDI SPI 写时序 (2字节地址, 1字节数据) |
| Section 4.1 | PDI 配置寄存器 (0x0140, 0x0150) |
