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

**单字节读 (CMD=0x03, 4 字节事务)**

```
CS LOW →
  MOSI: [A[12:5]] [A[4:0]<<3|0x03] [0xFF等待] [0xFF终止]
  MISO: [IRQ 0x0220] [IRQ 0x0221] [dummy   ] [数据    ]
→ 等 BSY=0 → CS HIGH  →  返回 MISO[3]
```

**块读 (CMD=0x02, 2+N 字节事务) ⭐**

```
CS LOW → NOP×3 (≥240ns) →
  MOSI: [A[12:5]] [A[4:0]<<3|0x02] [0x00]...[0x00] [0xFF]
  MISO: [IRQ 0x0220] [IRQ 0x0221] [d0  ]...[dN-2] [dN-1]
→ 等 BSY=0 → CS HIGH  →  返回 MISO[2..2+N-1]

关键规则: 只有最后一个数据字节是 0xFF，前面的都是 0x00
```

**写寄存器 (CMD=0x04)**

```
CS LOW →
  MOSI: [A[12:5]] [A[4:0]<<3|0x04] [数据...]
  MISO: [IRQ 0x0220] [IRQ 0x0221] [xx    ]
→ 等 BSY=0 → CS HIGH
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

| # | 问题 | 严重度 | 修复方式 |
|---|------|--------|---------|
| 1 | **CS 上电初始为低** — PA4 默认 RESET，上电瞬间选中 AX58100 | 中 | `MX_SPI1_Init` 末尾加 `SPI_CS_HIGH()` |
| 2 | **SPI_ReadBuffer 缓冲区越界** — size>128 时越界读取 | 高 | 加门禁 `if(size>128) return HAL_ERROR` |
| 3 | **ESC 块读写栈溢出** — `txBuf[259]+rxBuf[259]≈518字节` | 高 | 改为 `static`，`ESC_MAX_BLOCK_SIZE` 降至 128 |
| 4 | **块读所有数据字节均为 0xFF** — ESC 将第一个数据字节当作读终止，导致 `pdiErrCode=0x60`，`g_escError=7` | 🔴致命 | 只有最后一个字节 = 0xFF，前面的字节 = 0x00 |
| 5 | **SPI_CS_HIGH 时序过早** — HAL 收发返回时移位寄存器可能未完全发送，CS 提前拉高导致错误 | 高 | `SPI_CS_HIGH` 前加 `while(SPI_FLAG_BSY){}` 等待硬件空闲 |
| 6 | **块读 CMD=0x03 的 wait state 字节被误判为终止** — 0xFF 在 wait state 位置与数据位置语义冲突 | 🔴致命 | 块读改用 CMD=0x02（无等待字节），用 3 个 NOP 替代 wait |

### Bug #4 详析：块读终止字节错误

```c
// 错误写法 — 每个数据字节都是 0xFF
for (i = 0; i < size; i++)
    txBuf[3 + i] = 0xFF;   // 第1个字节 ESC 就认为"终止了"

// 正确写法 — 只有最后一个字节是 0xFF
for (i = 0; i < size - 1; i++)
    txBuf[2 + i] = 0x00;   // 非终止
txBuf[2 + size - 1] = 0xFF; // 最后一个才是终止
```

AX58100 数据手册规定：MOSI≠0xFF 时 ESC 认为后面还有数据；MOSI=0xFF 时 ESC 停止预取。单字节读不受影响（唯一的那个字节 = 最后一个 = 0xFF 正确），块读受影响严重。

### Bug #6 详析：CMD=0x03 vs CMD=0x02

CMD=0x03（带 wait state byte）在单字节读时正常，但在块读时存在语义问题—wait state 的 0xFF 和数据终止的 0xFF 无法区分。CMD=0x02 去掉 wait state byte，地址段后直接进入数据段，用 NOP 延时满足 t_read ≥ 240ns 的要求。

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

### 单字节读 0x0000 (CMD=0x03, 4 字节)

```
CS   ‾‾‾\_________________________/‾‾‾‾‾‾‾‾‾‾
SCK  ‾‾‾‾‾\___/‾\___/‾\___/‾\___/‾‾‾‾‾‾‾‾‾‾
MOSI ______/0x00\__/0x03\__/0xFF\__/0xFF\______
MISO ______/IRQ0 \__/IRQ1 \__/xx  \__/0xC8\______
         Byte0    Byte1    Wait    Data+Term
```

### 块读 0x1000 8 字节 (CMD=0x02, 10 字节)

```
CS   ‾‾‾\_________________________________/‾‾‾‾
SCK  ‾‾‾‾‾\___/‾\___/‾\___/‾\___/ ... /‾\___/‾
MOSI ______/0x80\__/0x02\__/0x00\__/0x00\ ... /0xFF\__
                NOP×3→                         ↑终止
MISO ______/IRQ0 \__/IRQ1 \__/d0  \__/d1  \ ... /d7 \__
         Byte0    Byte1    D0     D1          D7
```

### 写 0x0140 寄存器 (CMD=0x04, 3 字节)

```
CS   ‾‾‾\___________________/‾‾‾‾‾‾‾‾‾‾
SCK  ‾‾‾‾‾\___/‾\___/‾\___/‾‾‾‾‾‾‾‾‾‾
MOSI ______/0x0A\__/0x04\__/0x05\_______
MISO ______/IRQ0 \__/IRQ1 \__/xx  \_______
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

---

## 八、踩坑记录总结

> 标注说明：🔴 = 致命 bug，单个即可导致功能不可用；🟡 = 隐患，组合触发

| # | 标注 | 问题 | 现象 | 根因 |
|---|------|------|------|------|
| 1 | 🔴 | 块读所有数据字节=0xFF | g_escError=7, pdiErrCnt=0xFF | 0xFF 是 ESC 读终止命令，非普通 dummy |
| 2 | 🔴 | CMD=0x03 块读语义冲突 | 修了#1仍不对 | wait byte 0xFF 和数据字节 0xFF 同值不同义 |
| 3 | 🟡 | SPI_CS_HIGH 时序 | 零星 PDI Error | HAL返回时移位寄存器可能仍在发最后bit |
| 4 | 🟡 | pdiErrCnt 累积不清零 | 误以为修复无效 | 0x030D 只增不减，需断电清零 |
| 5 | 🟡 | CS 上电为低 | AX58100 意外选中 | CubeMX 默认 Output Low |
| 6 | 🟡 | 栈溢出风险 | 无崩溃但隐患大 | 块读写函数栈分配 518 字节 |

### Bug #1 和 #2 的关系

这两个 bug 叠加导致了块读完全不工作：

```
Bug #1: 所有数据字节=0xFF
  → 修了 → CMD=0x03 的 0xFF wait byte 又制造了一个"假终止"
  → 再修 → 换 CMD=0x02 + NOP 延时，彻底消除 0xFF 歧义
```

### 调试教训

1. **单字节测试先行** — 先用 `ESC_ReadRegister(0x0000)` 验证物理层，确认能读到 0xC8 后再搞块读写
2. **PDI Error Counter 必须先清零** — 断电重启或写 0 到 0x030D，否则旧错误混淆判断
3. **MOSI 的值有协议语义** — 不是随便发 0xFF 就行，每个 bit 的位置和值都可能触发 ESC 的特定行为
4. **数据手册的一句话不能跳过** — "If MOSI is low during a data byte transfer, at least one more byte will be read" 这句话就是全部问题的答案
