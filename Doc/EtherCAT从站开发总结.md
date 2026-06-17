# EtherCAT 从站开发总结

> AX58100 + STM32F103 · SPI PDI · 已完成第1~5步

---

## 一、项目概览

| 项目 | 说明 |
|------|------|
| 硬件 | AX58100（ASIX 2口 ESC）+ STM32F103VBT6（72MHz） |
| 接口 | SPI Mode 3, PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI, 4.5MHz |
| 工具链 | MDK-ARM V5.32, STM32CubeMX, HAL 库 |
| 主站 | 未接（自测验证通过），目标主站 TwinCAT |

### ESC 是什么

AX58100 不是传统 SPI 外设，而是一块"共享内存交换机"：

```
主站 (TwinCAT)  ←──网线──→  AX58100 (双端口RAM)  ←──SPI──→  STM32
                             9KB RAM
                             寄存器 0x0000~0x0FFF
                             过程数据 0x1000~0x1FFF
```

两边各写各的，不需要互相同步。ESC 硬件自动完成以太网帧的拼包拆包。

---

## 二、软件架构（三层分离）

```
App/app_ethercat.c/h    ← EtherCAT 应用层 (状态机、SM、CoE、过程数据)
        │
BSP/AX58100.c/h         ← ESC 驱动 (PDI 协议、寄存器读写、地址映射)
        │
Core/spi.c/h            ← SPI 硬件抽象 (CS 控制、字节收发、回环测试)
        │
STM32 HAL               ← 硬件层 (CubeMX 生成)
```

| 层 | 文件 | 对外提供 |
|----|------|---------|
| SPI 抽象 | `Core/spi.c/h` | `SPI_WriteByte`, `SPI_ReadByte`, `SPI_CS_LOW/HIGH` 等 |
| ESC 驱动 | `BSP/AX58100.c/h` | `ESC_ReadRegister`, `ESC_WriteRegister`, `ESC_ReadBlock`, `ESC_WriteBlock`, `ESC_ReadSMConfig`, `ESC_WriteSMConfig`, `AX58100_ReadESCInfo` |
| 应用层 | `App/app_ethercat.c/h` | `ECAT_Init`, `ECAT_MainTask`, `ECAT_GetState`, `ECAT_SelfTest` |

**设计原则**：每层只依赖下一层。应用层不调 `HAL_SPI_TransmitReceive`，只调 `ESC_ReadRegister`。

---

## 三、已完成内容（第1~5步）

### 第1步：SPI 物理层通信 ✅

- SPI Mode 3 (CPOL=1, CPHA=1), 4.5MHz, MSB first
- 软件 CS 控制，CS HIGH 前等 BSY=0（防"Read continued after termination"错误）
- `SPI_LoopbackTest`：自环回测试验证硬件

### 第2步：ESC 寄存器读写 ✅

- **CMD=0x03**：单寄存器读（Read + Wait State），地址段后 1 字节 wait state（8 个 SPI 时钟），满足 t_read ≥ 240ns
- **CMD=0x04**：写寄存器，地址段后直接跟数据
- **CMD=0x02**：块读（No Wait），无 wait state，拆为两次 SPI 传输，NOP 放在地址和数据之间

#### 踩坑：NOP 延时位置错误

最初 NOP 放在 `SPI_CS_LOW()` 之后、`HAL_SPI_TransmitReceive` 之前，不在 t_read 的路径上。4.5MHz 低速下地址段传输本身（3.56μs）远超 240ns，碰巧能工作。修复后拆为两次 HAL 调用：

```
CS LOW → HAL(addr,2) → NOP×10 → HAL(data,N) → CS HIGH
```
详见：`踩坑记录_NOP延时位置错误.md`

### 第3步：ESC 完整信息读取 ✅

`AX58100_ReadESCInfo()` 一次读齐所有身份/能力寄存器 → `g_escInfo`

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
Init (0x01) ⇄ PreOp (0x02) ⇄ SafeOp (0x04) ⇄ Op (0x08)
```

#### 核心寄存器

| 寄存器 | 地址 | 谁写 | 谁读 |
|--------|------|------|------|
| AL Control | 0x0120 | 主站（网线） | 从站（SPI） |
| AL Status | 0x0130 | 从站（SPI） | 主站（网线） |
| AL Status Code | 0x0134 | 从站（SPI） | 主站（网线） |

四个状态切换都走这对寄存器，只是 bit[3:0] 的值不同。

#### 核心函数

```c
ECAT_MainTask()              // 每 10ms: 读 AL Control → 检测请求 → 执行跳转 → 写 AL Status
  └── _ECAT_DoTransition()   // 核心跳转逻辑 (和 ECAT_SelfTest 共用)
        ├── IsValidTransition()  // 合法性校验
        ├── OnLeaveState()       // 离开旧状态
        └── OnEnterState()       // 进入新状态 → 触发 SM 初始化
```

#### 自测

`ECAT_SelfTest()` 跑 Init→PreOp→SafeOp→Op→SafeOp→PreOp→Init，返回 0 即通过。不需要网线。

详见：`第4步_状态机实现.md`

### 第5步：SyncManager 配置 ✅

4 个通道，每个 16 字节配置：

| SM | 地址 | 长度 | 模式 | 方向 | 用途 |
|----|------|------|------|------|------|
| SM0 | 0x1000 | 128B | 邮箱 | 主→从 | CoE 命令 |
| SM1 | 0x1080 | 128B | 邮箱 | 从→主 | CoE 响应 |
| SM2 | 0x1100 | 32B | 缓冲 | 主→从 | 过程数据输出 |
| SM3 | 0x1120 | 32B | 缓冲 | 从→主 | 过程数据输入 |

进入 PreOp 时自动调用 `ECAT_SM_Init()` 写入 ESC 硬件。

---

## 四、测试体系

```
上电 → AX58100_ReadESCInfo() → ECAT_Init() → ECAT_SelfTest() → 返回 0x00
```

| 测试 | 函数 | 验证内容 |
|------|------|---------|
| 自测 | `ECAT_SelfTest()` | 状态机 6 次跳转 + SM 配置 + 硬件寄存器同步 |
| 身份 | `ESC_TestReadID()` | Type=0xC8, SPI 通信正常 |
| 读写 | `ESC_TestReadWrite()` | ESC RAM 读写一致 |
| 全诊断 | `ESC_Diagnose()` | 身份+PDI错误+SM0+RAM+读写 |
| 自环回 | `SPI_LoopbackTest()` | MOSI/MISO 短接验证 |
| 波形 | `SPI_SendTestPattern()` | 0x55/0xAA 交替，示波器 |

---

## 五、寄存器定义使用情况

`AX58100.h` 中的宏，按使用状态分为三类：

### 正在用的（必须有）

| 宏 | 被谁用 |
|----|--------|
| `ESC_CMD_READ / READ_NOWAIT / WRITE` | 所有 ESC 读写函数 |
| `ESC_MAX_BLOCK_SIZE` | 块读写 buffer |
| `ESC_REG_TYPE ~ ESC_REG_FEATURES` | `AX58100_ReadESCInfo` |
| `ESC_REG_AL_CONTROL / AL_STATUS / AL_STATUS_CODE` | 状态机 |
| `ESC_REG_PDI_ERR_CNT / ERR_CODE` | `AX58100_ReadESCInfo`、`ESC_Diagnose` |
| `ESC_REG_SM_BASE / SM_STRIDE` | SM 读写函数 |
| `SM_OFF_*` | SM 读写函数 |
| `SM_DIR_* / SM_MODE_* / SM_CTRL_*` | SM 配置 + 默认布局 |
| `ESC_STATE_INIT / PREOP / SAFEOP / OP` | 状态机 |
| `ESC_RAM_BASE` | 测试函数 |

### 为后面步骤预留

| 宏 | 什么时候用 |
|----|-----------|
| `ESC_REG_FMMU_BASE / FMMU_STRIDE` | 第7步配 FMMU |
| `ESC_REG_DC_BASE` | 分布式时钟同步 |
| `AL_CTRL_ACK / ERROR_ACK` | 状态机更完善时 |
| `ESC_REG_AL_EVENT / MASK` | 中断驱动时 |

### 几乎用不到

| 宏 | 原因 |
|----|------|
| `ESC_REG_WDG_DIVIDER` | 看门狗一般不配 |
| `ESC_REG_STATION_ALIAS` | 主站通过网线配，不需要软件写 |
| `AL_STATUS_BOOT_NOT_SUPP` | Bootstrap 模式很少用 |

宏不占内存和编译开销，留着不用翻手册查地址。

---

## 六、进度

```
✅ 第1步: SPI 物理层通信
✅ 第2步: ESC 单寄存器/块读写
✅ 第3步: ESC 完整信息读取
✅ 第4步: 状态机
✅ 第5步: SyncManager 配置
⬜ 第6步: CoE 邮箱协议
⬜ 第7步: 过程数据
⬜ 第8步: ESI/XML 从站描述文件
```

---

## 七、相关文档

| 文档 | 内容 |
|------|------|
| `AX58100驱动原理简述.md` | ESC 硬件原理、寄存器分类、数据流向 |
| `ESC寄存器与SPI通信FAQ.md` | SPI 通信协议、CMD 编码、SM/FMMU 详解 |
| `AX58100协议手册.md` | AX58100 数据手册笔记 |
| `SPI修改日志.md` | 开发过程中的 bug 记录和修复 |
| `踩坑记录_NOP延时位置错误.md` | NOP 放置位置错误的分析和修复 |
| `第4步_状态机实现.md` | 状态机实现细节、AL Control/Status 详解、架构拆分 |

---

> 最后更新：2026-06-14
