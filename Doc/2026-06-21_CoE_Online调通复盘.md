# 2026-06-21 CoE Online 调通复盘

时间: 2026-06-21 凌晨  
项目: `EtherCat_Test`  
目标: 让 TwinCAT 的 `CoE - Online` 从 Offline/弹窗变成真正 Online。

## 1. 今晚最终结果

CoE 邮箱已经跑通。

现象确认:

- TwinCAT `CoE - Online` 能显示 `Online Data`。
- 取消 `Show Offline Data` 后不再卡在离线数据。
- TwinCAT 能在线读取对象字典:
  - `0x1000` Device Type
  - `0x1008` Device Name
  - `0x1009` Hardware Version
  - `0x100A` Software Version
  - `0x1018` Identity
  - `0x1600` RxPDO mapping
  - `0x1A00` TxPDO mapping
  - `0x1C00` Sync manager type
  - `0x1C12` RxPDO assign
  - `0x1C13` TxPDO assign
- `g_dbg_coe_rxCnt` 和 `g_dbg_coe_procCnt` 会随着 TwinCAT 读取对象持续增加。
- `g_dbg_sm1Status = 0x0222`，低字节 `0x22` 是真正的 SM1 邮箱 control，不再是之前误读到的 SM2 `0x64`。

一句话结论:

> 不是 CoE 对象字典没写好，而是 SM1 邮箱底层寻址和状态判断有错误。修正后，TwinCAT 才真正读到了从站回包。

## 2. 一开始的现象

TwinCAT 侧:

- `CoE - Online` 里取消 `Show Offline Data` 会弹窗。
- 弹窗内容类似: `Object 0x1000 could not be read`。
- 点 `Update List` 也读不到在线对象。

固件 Watch 侧:

- `g_dbg_txTimeout = 0`
- `g_dbg_reqIndex = 0x1000`
- `g_dbg_reqSubIndex = 0x00`
- `g_dbg_respCmd = 0x43`
- `g_dbg_respIndex = 0x1000`
- `g_dbg_respSubIndex = 0x00`
- `g_dbg_lastTxLen = 0x0010`
- `g_dbg_txMbxLen = 0x000A`
- 每点一次 TwinCAT，`g_dbg_coe_rxCnt` 和 `g_dbg_coe_procCnt` 会同步增加。

这些信息说明:

- 主站请求已经进了 SM0。
- CoE 主循环已经处理了请求。
- `0x1000:00` 的 SDO Upload 响应也组出来了。
- 响应内容本身基本正确。

所以问题不在对象字典，也不在 SDO Upload 组包，而在“响应有没有正确交给 SM1 邮箱，让 TwinCAT 读到”。

## 3. 修复一: SM1 邮箱写入方式

原来的 `ESC_Mbx_Write()` 会把整个 SM1 区都写满，例如 SM1 长度 128B，但一次 SDO 响应只有 16B，也会写 128B。

参考 Beckhoff SSC 的 `MBX_CopyToSendMailbox()` 后，改为:

- 只写实际响应长度。
- 写完后检查 SM1 full 标志。
- 如果 full 没置位，再补写 SM1 区末 2 字节触发 full。

修复点:

- 文件: `Bsp/Src/AX58100.c`
- 函数: `ESC_Mbx_Write()`

这个修改让底层行为更接近 SSC:

```c
status = ESC_WriteBlock(sm1Addr, pBuf, len);
...
ESC_WriteBlock(sm1Addr + sm1Len - 2U, trigger, 2);
```

但这一步做完后，问题还没彻底解决，因为更底层的 SM 寄存器地址还错着。

## 4. 修复二: SM full 状态位判断错误

之前代码把 SM status 当成 8 位状态寄存器来判断:

```c
SM_STATUS_MBX_FULL = 1 << 3
```

也就是判断 `0x08`。

但参考 SSC 后发现，邮箱 full 标志使用的是 16 位 control/status word:

```c
SM_STATUS_MBX_BUFFER_FULL = 0x0800
```

也就是说应该读取:

```c
word = control | (status << 8)
```

然后判断:

```c
word & 0x0800
```

修复点:

- 文件: `Bsp/Inc/AX58100.h`
  - `SM_STATUS_MBX_FULL` 改为 `0x0800U`
  - `ESC_SM_Config_t.status` 改为 `uint16_t`
- 文件: `Bsp/Src/AX58100.c`
  - `ESC_SM_MbxFull()` 改为读取 `SM_OFF_CONTROL` 开始的 2 字节。
  - SM0 读后解锁判断改为 16 位。
  - SM1 写后触发判断改为 16 位。

这个修复让 full 判断和 SSC 对齐。

## 5. 真正让它“突然通了”的关键: SM stride 错了

最关键的线索是这个 Watch 值:

```text
g_dbg_sm1Status = 0x0164 / 0x1164 / 0x2164 循环
```

这里低字节 `0x64` 非常异常。

原因:

- `0x64` 不是 SM1 邮箱 control。
- `0x64` 是 SM2 过程数据输出的 control。
- 正常 SM1 邮箱 control 应该是 `0x22`。

这说明代码所谓的“SM1”其实读到了 SM2。

最后查到原因:

```c
#define ESC_REG_SM_STRIDE 0x0010U
```

这个定义是错的。

AX58100/SSC 的 SyncManager 配置块是每通道 8 字节:

```text
SM0 = 0x0800
SM1 = 0x0808
SM2 = 0x0810
SM3 = 0x0818
```

之前按 `0x10` 步进时:

```text
SM0 = 0x0800
SM1 = 0x0810  实际读到 SM2
```

所以之前发生了很迷惑的情况:

- SM0 还能收到请求。
- CoE 也能处理请求。
- 响应也组对了。
- 但是 `ESC_Mbx_Write()` 读取的“SM1 地址/长度”其实来自 SM2。
- 响应被写到了错误的区域，TwinCAT 当然读不到 SM1 邮箱响应。

最终修复:

```c
#define ESC_REG_SM_STRIDE 0x0008U
```

修完后:

```text
g_dbg_sm1Status = 0x0222
```

低字节 `0x22` 正确，说明代码终于读到了真正的 SM1。

这就是为什么看起来“突然上线了”。

## 6. 今晚还做了哪些清理

CoE Online 跑通后，做了一轮代码收敛:

### 6.1 删除冗余调试变量

已删除或不再导出:

- `g_dbg_sm0RawSts`
- `g_dbg_coe_state`
- `g_dbg_skipReason`
- `g_dbg_sm0FullCnt`
- `g_dbg_rxMbxLen`
- `g_dbg_sdoInfoListType`
- `g_dbg_pdiErr`
- `g_dbg_irq0`
- `g_dbg_irq1`

保留核心 Watch 变量:

- 状态机:
  - `g_dbg_alCtrlLo`
  - `g_dbg_alStatus`
- SM 邮箱:
  - `g_dbg_sm0Addr`
  - `g_dbg_sm0Len`
  - `g_dbg_sm0Ctrl`
  - `g_dbg_sm0Status`
  - `g_dbg_sm1Addr`
  - `g_dbg_sm1Len`
  - `g_dbg_sm1Ctrl`
  - `g_dbg_sm1Status`
- CoE:
  - `g_dbg_coe_rxCnt`
  - `g_dbg_coe_procCnt`
  - `g_dbg_txTimeout`
  - `g_dbg_reqIndex`
  - `g_dbg_respIndex`
  - `g_dbg_lastTxLen`

### 6.2 更新注释

重点补了这些容易踩坑的注释:

- SyncManager 每通道是 8 字节，不是 16 字节。
- SM full 判断要读 16 位 control/status word。
- `SM_STATUS_MBX_FULL = 0x0800`。
- `ESC_Mbx_Write()` 只写实际响应长度，再补写末 2 字节触发 full。
- 当前进度更新为: CoE Online 已完成，下一步是 PDO 映射。

### 6.3 整理 Keil WatchWindow

`MDK-ARM/EtherCat_Test.uvoptx` 里的 Watch 列表也整理过:

- 删除旧变量。
- 删除重复项。
- 增加 `g_dbg_sm1Len`。
- 保留最常用的状态/SM/CoE 诊断变量。

## 7. 当前项目进度

已经完成:

- SPI PDI 单寄存器/块读写。
- AX58100 基础识别。
- EtherCAT 状态机。
- TwinCAT 能识别从站。
- 设备能进入 OP。
- EEPROM/ESI 基础匹配。
- SM0/SM1 邮箱通道跑通。
- CoE SDO Upload 跑通。
- CoE Online 能在线读取对象字典。

正在进入下一阶段:

- PDO 映射和过程数据交换。

下一步重点:

1. 核对 `0x1600` / `0x1A00` 映射内容。
2. 核对 `0x1C12` / `0x1C13` 分配内容。
3. 让 TwinCAT Process Data 的 Inputs/Outputs 和 SM2/SM3 实际数据完全一致。
4. 验证 OP 下:
   - TwinCAT 写 Output，MCU 能读到。
   - MCU 写 Input，TwinCAT 能看到。

## 8. 明天建议先做什么

建议顺序:

1. 先在 Keil 里完整 Build 一次。
2. 烧录后断电重启板子。
3. TwinCAT 扫描/Reload Device。
4. 看 Watch:

```text
g_dbg_sm0Addr   应为 0x1000
g_dbg_sm0Len    应为 0x0080
g_dbg_sm0Ctrl   应为 0x26

g_dbg_sm1Addr   应为 0x1080
g_dbg_sm1Len    应为 0x0080
g_dbg_sm1Ctrl   应为 0x22

g_dbg_txTimeout 应保持 0
```

5. 打开 `CoE - Online`，确认在线对象仍能读取。
6. 转到 `Process Data` / `Online` 页面，开始检查 PDO 过程数据映射。

## 9. 这次最重要的经验

不要只看“请求进来了、响应也组了”，邮箱还要确认三件事:

1. SM 寄存器寻址是否正确。
2. full/empty 判断是否按 ESC 的真实位宽判断。
3. 写响应后是否真的触发了 SM1 full，让主站能读走。

这次 CoE 迟迟不能 Online，根因不是高层 CoE，而是底层 SM1 指错了:

```text
错误: SM stride = 0x10, SM1 被当成 0x0810, 实际读到 SM2
正确: SM stride = 0x08, SM1 = 0x0808
```

修正后，TwinCAT 才真正从 SM1 读到了 SDO 响应，所以 CoE Online 突然上线。
