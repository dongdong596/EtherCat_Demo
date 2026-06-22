# 从 SPI 到 PDO 的 EtherCAT 从站开发逻辑梳理

本文按本工程当前代码结构梳理 EtherCAT 从站从 SPI 调通到 PDO 驱动应用的开发顺序。

工程中的主要分层如下：

```text
Core/Src/spi.c
    ↓
Bsp/Src/AX58100.c
    ↓
App/Src/app_ethercat.c
App/Src/app_coe.c
```

其中：

```text
spi.c          负责 STM32 SPI 和 CS 控制
AX58100.c      负责 ESC 寄存器、块读写、SM、Mailbox
app_coe.c      负责 CoE、SDO、对象字典 OD
app_ethercat.c 负责状态机、SM/FMMU 检查、PDO 过程数据
```

## 1. SPI 测试完成后做什么

SPI 能正常读取 AX58100 的 ESC 寄存器，例如版本号、类型号后，下一步不是马上做状态机，也不是马上做 CoE。

下一步是验证：

```text
ESC 单寄存器读写
ESC 块读写
ESC DPRAM/过程 RAM 访问
```

因为后面的 SM、Mailbox、PDO 都不是只读一个寄存器，而是连续访问一段 ESC 地址空间。

本工程对应函数：

```c
ESC_ReadRegister()
ESC_WriteRegister()
ESC_ReadBlock()
ESC_WriteBlock()
```

结论：

```text
SPI 单寄存器读成功
    ↓
必须继续验证 ESC_ReadBlock / ESC_WriteBlock
    ↓
块读写稳定后才能进入 SM、Mailbox、CoE、PDO
```

## 2. SM 配置和状态管理

ESC 寄存器和块读写稳定后，下一步是 SyncManager，也就是 SM。

SM 的作用是规定 ESC 内部哪块 RAM 用来收发哪类数据。

本工程默认设计：

| SM | 用途 | 起始地址 | 长度 | 控制字 | 激活 |
|---|---|---:|---:|---:|---:|
| SM0 | 邮箱：主站到从站 | `0x1000` | `0x0080` | `0x26` | `0x01` |
| SM1 | 邮箱：从站到主站 | `0x1080` | `0x0080` | `0x22` | `0x01` |
| SM2 | PDO 输出：主站到从站 | `0x1100` | `0x0020` | `0x64` | `0x01` |
| SM3 | PDO 输入：从站到主站 | `0x1120` | `0x0020` | `0x20` | `0x01` |

读取 SM 配置的函数：

```c
ESC_SM_ReadConfig(uint8_t smIdx, ESC_SM_Config_t *pCfg)
```

每个 SM 配置块是 8 字节：

```text
offset 0x00~0x01：起始地址
offset 0x02~0x03：长度
offset 0x04：控制字 control
offset 0x05：状态 status
offset 0x06：激活 activate
offset 0x07：PDI control
```

代码里把 control 和 status 拼成一个 16 位值：

```text
status_word = control | (status << 8)
```

也就是：

```text
低字节 = control
高字节 = SM status
```

注意：

```text
control 是配置：这个 SM 怎么工作
status 是状态：这个 SM 当前发生了什么
activate 是激活：这个 SM 是否启用
```

判断 mailbox/buffer full 用的是：

```text
status 高字节 bit3
= 16 位 status_word 的 bit11
= 0x0800
```

代码里：

```c
#define SM_STATUS_MBX_FULL 0x0800U
```

激活状态不是 status 里的位，而是：

```text
offset 0x06 的 bit0
```

通常看到：

```text
activate = 0x01
```

就表示该 SM 已激活。

SM status 是 ESC 硬件自动更新的。主站或 MCU 对 SM 对应 RAM 区读写后，ESC 会自动改变 status 位，MCU 不应该手动写 status。

## 3. Mailbox 通信

SM 管理完成后，下一步先做 Mailbox，而不是直接做 PDO。

Mailbox 使用：

```text
SM0：主站发命令给从站
SM1：从站回复主站
```

底层函数在 `AX58100.c`：

```c
ESC_Mbx_RxFull()
ESC_Mbx_Read()
ESC_Mbx_TxFull()
ESC_Mbx_Write()
```

流程：

```text
主站写 SM0
    ↓
ESC 硬件置 SM0 full
    ↓
ESC_Mbx_RxFull() 检测到 SM0 有数据
    ↓
ESC_Mbx_Read() 从 SM0 读出 Mailbox 包
    ↓
上层解析 Mailbox 内容
    ↓
ESC_Mbx_Write() 把响应写入 SM1
    ↓
ESC 硬件置 SM1 full
    ↓
主站从 SM1 读走响应
```

读 SM0 后，如果 full 没清掉，代码会补读 SM0 末尾 2 字节，让 ESC 确认 MCU 已经访问到邮箱末尾。

写 SM1 后，如果 full 没置位，代码会补写 SM1 末尾 2 字节，让 ESC 确认从站响应已经准备好。

## 4. Mailbox、CoE、SDO 的关系

Mailbox 是通道，SDO 是通道里装的一种业务命令。

层次是：

```text
SM0 / SM1
    ↓
Mailbox
    ↓
CoE
    ↓
SDO
    ↓
Object Dictionary
```

Mailbox 只负责收发包：

```text
有没有包
包多长
从哪里读
往哪里写
```

SDO 负责解释包里的业务：

```text
读哪个 index/subindex
写哪个 index/subindex
对象是否存在
对象是否可读写
返回什么数据
```

一包 Mailbox 数据拆分如下：

```text
rxBuf[0..5]    Mailbox Header
rxBuf[6..7]    CoE Header
rxBuf[8..15]   SDO Header
rxBuf[16...]   SDO Data / 额外数据
```

代码中的偏移：

```c
#define MBX_HDR_SIZE 6
#define COE_OFF      6
#define SDO_OFF      8
```

判断过程：

```text
先看 Mailbox Header 的 typeCounter 低 4 位
    ↓
如果等于 0x03，说明是 CoE
    ↓
再看 CoE Header 的 service
    ↓
如果 service = 0x02，说明是 SDO Request
    ↓
再解析 SDO Header 的 command / index / subindex / data
```

例如主站读设备名：

```text
Mailbox type = CoE
CoE service  = SDO Request
SDO command  = Upload Request
index        = 0x1008
subindex     = 0x00
```

## 5. OD 对象字典的作用

OD 是 Object Dictionary，对象字典。

代码中的 `OD_` 系列函数用于根据 `index + subindex` 查找对象。

例如主站读：

```text
0x1008:00
```

代码调用：

```c
OD_Find(0x1008, 0)
```

找到对象字典里的设备名称，然后通过 SDO 响应返回给主站。

常见 OD 函数：

```c
OD_Find()
OD_GetMaxSubIndex()
OD_GetObjectCode()
OD_GetObjectDataType()
OD_GetAccessFlags()
OD_GetBitLength()
```

普通变量对象默认子索引为 0：

```text
0x1008:00 Device Name
0x1000:00 Device Type
0x1009:00 Hardware Version
```

结构对象才有多个子索引：

```text
0x1018:00 最大子索引
0x1018:01 Vendor ID
0x1018:02 Product Code
0x1018:03 Revision
0x1018:04 Serial Number
```

## 6. 从站如何组 SDO 响应

以主站读 `0x1008:00` 为例：

```text
主站通过 SM0 发 SDO Upload Request
    ↓
CoE_MainTask() 解析出 index = 0x1008, subindex = 0
    ↓
SDO_HandleUpload()
    ↓
OD_Find(0x1008, 0)
    ↓
找到 g_deviceName
    ↓
组 Mailbox Header
    ↓
组 CoE Header = SDO Response
    ↓
组 SDO Header + 数据
    ↓
等待 SM1 空闲
    ↓
ESC_Mbx_Write()
    ↓
主站从 SM1 读走响应
```

如果对象不存在，返回 SDO Abort。

如果对象存在但不可读，也返回 SDO Abort。

如果数据长度小于等于 4 字节，走 expedited upload。

如果数据长度大于 4 字节，例如字符串，走 normal upload，后续可能分段上传。

## 7. PDO 映射对象的含义

CoE/SDO 通了以后，下一步是 PDO 映射。

SDO 是非实时对象访问：

```text
主站偶尔读写对象字典
```

PDO 是实时过程数据：

```text
主站周期性和从站交换数据
```

本工程当前 PDO 相关对象：

```text
0x1600  RxPDO Mapping
0x1A00  TxPDO Mapping
0x1C12  RxPDO Assign
0x1C13  TxPDO Assign
0x2000  主站输出对象
0x2001  从站输入对象
```

其中：

```text
0x2000 / 0x2001 是真正的数据对象
0x1600 / 0x1A00 是 PDO 里装什么对象的说明书
0x1C12 / 0x1C13 是哪个 SM 绑定哪个 PDO Mapping 的分配表
```

当前工程：

```text
0x1C12:01 = 0x1600
0x1600:01 = 0x20000020
```

意思是：

```text
SM2 使用 0x1600 这个 RxPDO Mapping
0x1600 里面放 0x2000:00，占 32bit
```

反方向：

```text
0x1C13:01 = 0x1A00
0x1A00:01 = 0x20010020
```

意思是：

```text
SM3 使用 0x1A00 这个 TxPDO Mapping
0x1A00 里面放 0x2001:00，占 32bit
```

## 8. Mapping 和 Assign 的关系

Mapping 可以包含多个实际对象。

Assign 也可以包含多个 PDO Mapping。

### 一个 Mapping 里有多个对象

例如：

```text
0x1600:00 = 2
0x1600:01 = 0x20000010   // 0x2000:00, 16bit
0x1600:02 = 0x20010010   // 0x2001:00, 16bit
```

意思是：

```text
0x1600 这个 RxPDO 里有两个对象
第一个对象占 16bit
第二个对象占 16bit
```

SM2 中排列为：

```text
SM2[0..1] → 0x2000:00
SM2[2..3] → 0x2001:00
```

### 一个 Assign 里有多个 Mapping

例如：

```text
0x1C12:00 = 2
0x1C12:01 = 0x1600
0x1C12:02 = 0x1601
```

意思是：

```text
SM2 使用两个 RxPDO Mapping
先使用 0x1600
再使用 0x1601
```

假设：

```text
0x1600:00 = 2
0x1600:01 = 0x20000010
0x1600:02 = 0x20010010

0x1601:00 = 1
0x1601:01 = 0x20020020
```

最终 SM2 中排列为：

```text
SM2[0..1] → 0x2000:00
SM2[2..3] → 0x2001:00
SM2[4..7] → 0x2002:00
```

规则：

```text
Assign 顺序决定 PDO Mapping 的拼接顺序
Mapping 顺序决定具体对象的排列顺序
```

## 9. PDO 数据如何拆装

PDO 映射对象不是数据缓冲区，而是数据说明书。

真正每周期搬运数据的是：

```text
SM2 RAM
SM3 RAM
```

本工程：

```text
SM2 = 0x1100，主站输出，从站读取
SM3 = 0x1120，从站输入，主站读取
```

如果 Mapping 是：

```text
0x1600:00 = 2
0x1600:01 = 0x20000020   // 32bit
0x1600:02 = 0x20010020   // 32bit
```

那么 SM2 是 8 字节：

```text
SM2[0..3] → 0x2000:00
SM2[4..7] → 0x2001:00
```

主站发送：

```text
0x2000:00 = 0x0000000F
0x2001:00 = 0x00000001
```

SM2 小端排列：

```text
SM2[0] = 0x0F
SM2[1] = 0x00
SM2[2] = 0x00
SM2[3] = 0x00

SM2[4] = 0x01
SM2[5] = 0x00
SM2[6] = 0x00
SM2[7] = 0x00
```

MCU 需要按映射顺序手动拆：

```c
obj2000 = ((uint32_t)m_pdOutput[0])
        | ((uint32_t)m_pdOutput[1] << 8)
        | ((uint32_t)m_pdOutput[2] << 16)
        | ((uint32_t)m_pdOutput[3] << 24);

obj2001 = ((uint32_t)m_pdOutput[4])
        | ((uint32_t)m_pdOutput[5] << 8)
        | ((uint32_t)m_pdOutput[6] << 16)
        | ((uint32_t)m_pdOutput[7] << 24);
```

反方向 TxPDO 也是一样。MCU 按 `0x1A00` 的映射顺序把对象打包进 SM3。

结论：

```text
映射表不会自动帮 MCU 拆装变量
MCU 必须按 Mapping 顺序手动 unpack / pack
```

## 10. 状态机与 OP 前检查

EtherCAT 状态切换顺序：

```text
INIT → PRE-OP → SAFE-OP → OP
```

进入 OP 之前不是先做 PDO 数据交换，而是先做配置检查。

本工程中：

```text
PRE-OP → SAFE-OP
    ↓
检查 SM0/SM1 邮箱配置
检查 SM2/SM3 过程数据配置

SAFE-OP → OP
    ↓
再次检查 SM2/SM3
检查 FMMU 是否映射到 SM2/SM3
```

只有检查通过后：

```text
m_currentState = OP
```

然后 `ECAT_ProcessDataExchange()` 才真正读 SM2、写 SM3。

虽然主循环里一直调用：

```c
ECAT_ProcessDataExchange();
```

但函数开头有判断：

```c
if (m_currentState != ESC_STATE_OP) return;
```

所以：

```text
未进入 OP 时不会做 PDO 交换
进入 OP 后才开始周期性 PDO 交换
```

## 11. OP 前和 OP 后分别做什么

OP 前：

```text
状态机切换
SM 配置
Mailbox 通信
CoE/SDO 对象字典访问
PDO 映射读取
FMMU 配置检查
```

OP 后：

```text
主站周期写 SM2
从站周期读 SM2
从站周期写 SM3
主站周期读 SM3
应用层根据 PDO 数据执行动作
```

在本工程中，OP 后的数据交换函数是：

```c
ECAT_ProcessDataExchange()
```

当前逻辑：

```text
SM2[0..3] → g_testCounter → LED 输出
本地输入 → g_testStatus → SM3[0..3]
```

## 12. PDO 驱动应用后是否算通

如果已经验证：

```text
主站能识别从站
能进入 PRE-OP / SAFE-OP / OP
CoE/SDO 能读取对象字典
PDO 映射正确
FMMU 检查通过
TwinCAT 写 Output 后 MCU 能读到
MCU 写 Input 后 TwinCAT 能看到
应用 IO 能被 PDO 驱动
```

那么可以认为最小 EtherCAT 从站链路已经跑通。

完整链路是：

```text
主站
  ↓
EtherCAT 网络
  ↓
AX58100 ESC
  ↓
SPI
  ↓
STM32
  ↓
应用 IO
```

后续增强方向：

```text
1. 增加更多 PDO 映射对象
2. 完善看门狗和错误处理
3. 增加 Sync0 / DC 同步
4. 做断线重连和长时间稳定性测试
5. 把测试 LED/开关替换成真实业务逻辑
```

如果只是普通 IO 从站，DC/Sync0 可以后做。

如果后续做伺服、运动控制、高速采样、多轴同步，就需要进一步实现 DC 和 Sync0 中断同步。

