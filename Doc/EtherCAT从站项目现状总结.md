# EtherCAT 从站项目现状总结

> 生成日期：2026-06-16
> 硬件：AX58100 ESC + STM32F103VBT6
> 开发工具：Keil MDK-ARM, STM32CubeMX

---

## 一、项目概况

| 项 | 内容 |
|---|---|
| ESC 芯片 | AX58100（ASIX，2 端口 ESC，含 9KB 双端口 RAM） |
| MCU | STM32F103VBT6（Cortex-M3, 72MHz, 128KB Flash） |
| 通信接口 | SPI Mode 3, 4.5MHz |
| 引脚 | PA4=CS_SCS_ESC, PA5=SCK, PA6=MISO, PA7=MOSI |
| 目标主站 | TwinCAT（Windows PC） |
| 当前可跑 | 自测全部通过，SOEM slaveinfo 能初始化网卡但未扫到从站 |

---

## 二、软件架构

```
App/app_ethercat.c/h     ←  EtherCAT 应用层
        │                      状态机 / CoE / 过程数据交换
        │ 调用 ESC_ReadRegister, ESC_WriteRegister, ESC_ReadBlock 等
BSP/AX58100.c/h          ←  ESC 驱动层
        │                      PDI 协议 / 寄存器读写 / SM 管理 / 看门狗
        │ 调用 SPI_WriteReadBuffer, SPI_CS_LOW, SPI_CS_HIGH
Core/spi.c/h             ←  SPI 硬件抽象层
        │                      CS 控制 / 字节收发 / 测试
        │ 调用 HAL_SPI_TransmitReceive
STM32 HAL                 ←  硬件层 (CubeMX 生成)
```

设计原则：每层只依赖下一层。App 不调 `HAL_SPI_TransmitReceive`，只调 BSP 层 API。

---

## 三、已完成内容

### 第1步：SPI 物理层 ✅

- SPI Mode 3 (CPOL=1, CPHA=1), 4.5MHz, MSB first
- 软件 CS 控制（PA4），CS HIGH 前等待 `SPI_FLAG_BSY` 清除
- `SPI_LoopbackTest()`：短接 MOSI/MISO 自环回验证
- `SPI_SendTestPattern()`：0x55/0xAA 交替，示波器验证波形

### 第2步：ESC 寄存器读写 ✅

| 命令 | 编码 | 用途 | 帧格式 |
|------|------|------|--------|
| CMD=0x03 | Read + Wait | 单字节读 | 4 字节帧：addr0 + addr1 + wait(0xFF) + data/term(0xFF) |
| CMD=0x04 | Write | 单/块写 | addr0 + addr1 + data... |
| CMD=0x02 | Read No Wait | 块读 | 拆两次 HAL：第一次发 2B 地址 → NOP×10 → 第二次发 N 字节数据 |

块读关键设计：拆为两次 HAL 调用，CS 全程拉低，NOP 延时放在地址和数据之间（满足 ESC 的 t_read ≥ 240ns）。

地址编码：13 位地址 + 3 位命令 → 2 字节地址段。

```
Byte0 = A[12:5]
Byte1 = A[4:0] << 3 | CMD[2:0]
```

每次 SPI 事务 MISO 自动带回 AL Event Request 寄存器的值，缓存到 `g_esc_irq0/1`。

### 第3步：ESC 完整信息 ✅

`AX58100_ReadESCInfo()` 一次读齐所有身份/能力寄存器 → `g_escInfo`：

| 字段 | 寄存器 | AX58100 实测值 |
|------|--------|---------------|
| Type | 0x0000 | 0xC8 |
| Revision | 0x0001 | 0x00 |
| FMMU Supported | 0x0004 | 0x08（8 通道） |
| SM Supported | 0x0005 | 0x08（8 通道） |
| RAM Size | 0x0006 | 0x09（9KB） |
| MAC | 0x0010 | 全 0（缺 EEPROM） |

### 第4步：状态机 ✅

实现 Beckhoff ETG.1000 标准状态跳转：

```
(上电) → Init ⇄ PreOp ⇄ SafeOp ⇄ Op
            ↑        ↑        ↑
         可配寄存器  可邮箱    可过程数据
```

**握手协议**：主站写 `AL Control`（从站通过 SPI 读），从站写 `AL Status`（主站通过网线读）。全程只用这两个寄存器，靠 bit[3:0] 值不同区分 Init/PreOp/SafeOp/Op。

每个状态切换分**两轮握手**（以 Init→PreOp 为例）：

```
第1轮 — ACK 确认：
  主站写 AL Control = 0x02 | 0x10 = 0x12   （目标 PreOp，ACK=1，叫你确认）
  从站处理后回 AL Status = 0x02 | 0x10 = 0x12  （当前 PreOp，Response=1，我确认好了）

第2轮 — 正式切：
  主站写 AL Control = 0x02 | 0x00 = 0x02   （目标 PreOp，ACK=0，正式切）
  从站正式进入 PreOp，回 AL Status = 0x02 | 0x00 = 0x02

为什么两轮？主站第一轮发 ACK=1 试探从站是否在线，收不到 Response 就超时重试，避免"主站以为切好了但从站掉线"的通信错乱。
```

对应的代码逻辑：

```c
// ECAT_MainTask() 读 AL Control
requestedState = alCtrlLo & 0x0F;   // bit3:0 → 目标状态
ackBit         = alCtrlLo & 0x10;   // bit4   → 主站要你确认吗

_ECAT_DoTransition(requestedState, ackBit);

// _ECAT_DoTransition() 写 AL Status 时：
statusLo = m_currentState;          // bit3:0 → 当前状态
if (ackBit) statusLo |= 0x10;       // bit4   → ACK=1 时回应 Response=1
ESC_WriteRegister(AL_STATUS, statusLo);
```

核心函数：

```c
ECAT_MainTask()              // 每 10ms: 读 AL Control → 执行跳转 → 写 AL Status
  └── _ECAT_DoTransition()   // 核心跳转逻辑 (ECAT_MainTask 和 ECAT_SelfTest 共用)
        ├── IsValidTransition()   // 合法性校验
        ├── OnLeaveState()        // 离开旧状态
        └── OnEnterState()        // 进入新状态 → PREOP 时触发 SM 初始化
```

**自测**：`ECAT_SelfTest()` 跑 Init→PreOp→SafeOp→Op→SafeOp→PreOp→Init，同时检查软件状态和硬件 AL Status 寄存器，返回 0 即通过。

### 第5步：SM 配置 + 看门狗 + 过程数据 ✅

SM 默认布局（无主站自测用）：

| SM | 物理地址 | 大小 | 模式 | 方向 | 用途 |
|----|---------|------|------|------|------|
| SM0 | 0x1000 | 128B | 邮箱 | M→S | CoE 命令接收 |
| SM1 | 0x1080 | 128B | 邮箱 | S→M | CoE 响应发送 |
| SM2 | 0x1100 | 32B | 缓冲 | M→S | 过程数据输出 |
| SM3 | 0x1120 | 32B | 缓冲 | S→M | 过程数据输入 |

- `ESC_SM_Init()` — PreOp 时一键配好 SM0~SM3
- `ESC_SM_ReadConfig()` — 块读 8 字节，回读单个 SM 完整配置
- `ESC_SM_Config()` — 逐寄存器写入单个 SM 配置

看门狗：`ESC_Watchdog_Config()` 禁用 PDI/SM 看门狗（开发阶段防止异常复位）。

过程数据：

```c
ECAT_ProcessDataExchange()   // OP 态每周期执行
  ├── ESC_ReadOutputData()   // 读 SM2 (主站→从站)
  ├── m_pdInput[0]++          // 心跳计数器
  └── ESC_WriteInputData()   // 写 SM3 (从站→主站)
```

---

## 四、关键踩坑记录

| # | 严重度 | 问题 | 根因 | 状态 |
|---|--------|------|------|------|
| 1 | 🔴致命 | 块读 8 字节，7 个错误 | 所有数据字节都是 0xFF，ESC 读到第一个就停止预取 | ✅ 已修 |
| 2 | 🔴致命 | 修了 #1 仍不对 | CMD=0x03 的 wait state byte(0xFF) 和数据终止 byte(0xFF) 语义冲突 | ✅ 已修，换 CMD=0x02 |
| 3 | 🔴致命 | TwinCAT 从 Init 切到 PreOp 超时 | 状态机缺 ACK/Response 握手 | ✅ 已修 |
| 4 | 🔴致命 | AL Status 归 0x0000 | SM 看门狗在无过程数据交换时超时复位芯片 | ✅ 已修，禁用看门狗 |
| 5 | 🟡隐患 | NOP 放 CS LOW 之后不在 t_read 路径上 | 低速下地址段本身 3.56μs 远超 240ns，碰巧能工作，高速会暴露 | ✅ 已修，拆两次 HAL |
| 6 | 🟡隐患 | CS 上电为低 | CubeMX 默认 PA4 Output Low | ✅ 已修 |
| 7 | 🟡隐患 | 栈溢出风险 | 块读写函数栈分配 518 字节 | ✅ 已修，改 static |
| 8 | 🟡注意 | 自测写 AL Control 失败 | AL Control(0x0120) PDI 只读，只能主站通过网线写 | ✅ 设计确认 |

---

## 五、待完成

### 第6步：CoE 邮箱协议 ⬜

TwinCAT 在 PreOp 阶段通过 SM0/SM1 发 CoE SDO 请求，查询从站设备信息。需要实现的最小子集：

**基础设施：**
- SM0 中断检测（ESC 收到数据后触发 IRQ → AL Event Request）
- 读 SM0 邮箱头（6 字节：length + address + channel + priority + type + counter）
- 通过 SM1 写响应

**需要响应的 SDO 请求：**

| Index | 名称 | 必要程度 |
|-------|------|---------|
| 0x1000 | Device Type | 必须 |
| 0x1008 | Device Name | 必须 |
| 0x1009 | Hardware Version | 建议 |
| 0x100A | Software Version | 建议 |
| 0x1018 | Identity (Vendor ID / Product Code / Revision / Serial) | 必须 |
| 0x1C00 | Sync Manager Communication Type | 必须 |
| 0x1C12 | RxPDO Assign | 必须 |
| 0x1C13 | TxPDO Assign | 必须 |

**需要实现的 SDO 命令：**

| 命令码 | 名称 | 用途 |
|--------|------|------|
| 0x40 | Upload Initiate | 读单个对象（主站查询设备信息） |
| 0x50 | Upload Initiate CA | 批量读（Complete Access） |
| 0x23 | Download Expedited | 写 ≤4 字节（SM/FMMU 配置值通常很小） |
| 0x21 | Download Initiate | 写 >4 字节（PDO Assign 等，需分段传输） |
| 0x80 | Abort | 拒绝不支持的请求 |

**要处理的特殊情况：**
- 数据超过 4 字节需要分段传输：下载（主→从）走 0x21→0x00 段，上传（从→主）走 0x40→0x60 段
- SM0 邮箱可能一次收多个命令（需循环处理直到 SM0 缓冲区空）
- SM1 写完响应后要更新 SM1 状态寄存器，通知 ESC"有新数据可以取走"

### 第7步：FMMU 过程数据映射 ⬜

FMMU 由**主站**通过 CoE SDO 下载写入 ESC 的 0x0600 寄存器，从站不需要主动配 FMMU，只需要在 CoE 邮箱层正确响应主站的 SDO Download 请求即可。

主站配置完成后，实际的数据流向：

```
主站逻辑地址 0x0000  ──FMMU0──► 物理 SM2 缓冲区  ──► STM32 读走
主站逻辑地址 0x0020  ──FMMU1──► 物理 SM3 缓冲区  ◄── STM32 写入
```

从站需要做的事：
- 更新 `ECAT_ProcessDataExchange()`，从 SM2/SM3 的实际地址（可能被主站改写）读写数据
- SafeOp → OP 状态切换时，正确响应主站的 FMMU 相关 SDO 下载请求

### 第8步：ESI/XML 从站描述文件 ⬜

最小 ESI XML 需要定义：

```xml
<Vendor>
  <Id>#x????????</Id>        <!-- Vendor ID，需向 ETG 申请 -->

<Descriptions>
  <Devices>
    <Device>
      <Type ProductCode="#x????????" RevisionNo="#x????????" />
      <Name>AX58100-STM32F103</Name>
      <Sm>                     <!-- SM0~SM3 布局 -->
      <RxPdo>                  <!-- 输出过程数据 (主站→从站) -->
      <TxPdo>                  <!-- 输入过程数据 (从站→主站) -->
      <Mailbox>                <!-- CoE 邮箱配置 -->
```

需要和 CoE 对象字典中的值一致。TwinCAT 从 ESI 文件读取设备布局，按布局发 SDO 请求。

---

## 六、文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `App/Inc/app_ethercat.h` | 45 | 状态机 API 声明 |
| `App/Src/app_ethercat.c` | 244 | 状态机核心 + 过程数据交换 |
| `Bsp/Inc/AX58100.h` | 350 | ESC 寄存器宏 + SM 宏 + API 声明 |
| `Bsp/Src/AX58100.c` | 597 | PDI 协议 + 寄存器读写 + SM + 看门狗 |
| `Core/Inc/spi.h` | 88 | SPI 函数声明 + CS 宏 |
| `Core/Src/spi.c` | 235 | SPI 基础收发 + 测试函数 |
| `Core/Inc/gpio.h` | 50 | GPIO 声明 |
| `Core/Src/gpio.c` | 88 | GPIO 初始化 |
| `Core/Inc/main.h` | 70 | 主函数声明 |
| `Core/Src/main.c` | 223 | 主循环 + 测试菜单 |
| `Core/Inc/stm32f1xx_it.h` | 69 | 中断声明 |
| `Core/Src/stm32f1xx_it.c` | 246 | 中断处理（EXIT2/3/10） |
| `Core/Src/stm32f1xx_hal_msp.c` | 86 | HAL MSP 初始化 |
| `Core/Src/system_stm32f1xx.c` | 407 | 系统时钟 |
| `Core/Inc/stm32f1xx_hal_conf.h` | 392 | HAL 模块配置 |
| `MDK-ARM/startup_stm32f103xb.s` | 306 | 启动文件 + 向量表 |
| **合计** | **~3500** | |

文档（中文）：

| 文件 | 内容 |
|------|------|
| `AX58100协议手册.md` | SPI PDI 协议详解 + 寄存器速查 + 踩坑完整记录 |
| `AX58100驱动原理简述.md` | ESC 硬件原理、"共享内存交换机"比喻 |
| `ESC寄存器与SPI通信FAQ.md` | 单读 vs 块读、地址编码、SM/FMMU 详解 |
| `EtherCAT从站开发总结.md` | 项目总览、三层架构、进度、测试体系 |
| `第4步_状态机实现.md` | 状态机实现细节 + 面试话术 |
| `SPI修改日志.md` | 开发调试全记录、6 个 Bug 的详细分析 |
| `踩坑记录_NOP延时位置错误.md` | NOP 放置错误的技术分析 + 修复 |
| `代码修改记录_2026-06-15.md` | 6 月 15 日修改详情（ACK 握手、看门狗、过程数据） |
| `调试记录_2026-06-15.md` | SOEM 调试记录 |
| `明日计划_2026-06-16.md` | 6 月 16 日工作计划 |

---

## 七、调用关系总览

```
main() 上电:
  ├─ AX58100_ReadESCInfo()    ← 读 ESC 完整信息 → g_escInfo
  ├─ ECAT_Init()              ← 状态机 = INIT, 写 AL Status
  └─ ESC_Watchdog_Config()    ← 禁用 PDI/SM 看门狗

main() while(1), 每 10ms:
  ├─ ECAT_MainTask()          ← 读 AL Control → 检测主站请求
  │     └─ _ECAT_DoTransition(state, ackBit)
  │           ├─ IsValidTransition()
  │           ├─ OnEnterState(PREOP) → ESC_SM_Init()
  │           ├─ OnEnterState(OP)    → 初始化 m_pdInput/m_pdOutput
  │           └─ ESC_WriteRegister(AL_STATUS)
  │                               Response 位 = ACK 位
  ├─ ECAT_ProcessDataExchange()  ← OP 态激活
  │     ├─ ESC_ReadOutputData()  ← 读 SM2 (M→S)
  │     └─ ESC_WriteInputData()  ← 写 SM3 (S→M)
  └─ HAL_Delay(10)
```

---

## 八、下一步建议

```
第6步: CoE 邮箱协议 (最关键，TwinCAT 在 PreOp 阶段用)
  └── 实现后应该能走到 SAFEOP/OP

第8步: ESI 文件 (可并行做，告诉 TwinCAT 设备布局)
  └── 做了此步 TwinCAT 知道该发哪些 SDO 请求

第7步: FMMU 过程数据映射 (最后，OP 态全速数据交换)
  └── 做成后 TwinCAT 能正常读写过程数据
```
