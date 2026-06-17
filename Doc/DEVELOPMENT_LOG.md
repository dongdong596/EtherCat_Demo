# EtherCAT 从站开发日志

> 项目: AX58100 EtherCAT 从站  
> 日期: 2026-06-17  
> 作者: dongdong596

---

## 一、当前进度

| 功能模块 | 状态 | 说明 |
|---------|------|------|
| SPI PDI 通信 | ✅ 完成 | 单寄存器读写 + 块读写，IRQ 状态自动捕获 |
| ESC 信息读取 | ✅ 完成 | 5 次 SPI 事务读齐身份/MAC/PDI 状态 |
| SyncManager 配置 | ✅ 完成 | SM0~SM3 一键初始化 + 逐通道读写 |
| EtherCAT 状态机 | ✅ 完成 | Init→PreOp→SafeOp→Op 全链路，TwinCAT 实测通过 |
| 过程数据交换 | ✅ 完成 | SM2/SM3 缓冲模式，OP 态每周期刷新 |
| CoE 对象字典 | ✅ 代码完成，待 ESI | SDO Upload/Download 已实现，需 ESI 文件配合 |
| CoE 邮箱通信 | ⬜ 待验证 | 需主站有 ESI 文件才会发起 SDO 请求 |
| ESI/XML 从站描述 | ⬜ 待开发 | 缺少导致 CoE Online 显示 offline |
| 看门狗 | ✅ 完成 | PDI/SM 看门狗已禁用 |

---

## 二、文件清单

```
App/Inc/app_coe.h          — CoE 协议栈头文件 (邮箱头/CoE头/SDO头/对象字典数据结构)
App/Src/app_coe.c          — CoE 协议栈实现 (对象字典 + SDO Upload/Download + 主循环)
App/Inc/app_ethercat.h     — EtherCAT 应用层头文件
App/Src/app_ethercat.c     — EtherCAT 状态机 + 过程数据交换
Bsp/Inc/AX58100.h          — AX58100 ESC 驱动头文件 (寄存器映射/SM/API)
Bsp/Src/AX58100.c          — AX58100 ESC 驱动实现 (SPI传输/SM管理/邮箱底层)
Core/Src/main.c            — 主程序入口 (初始化 + 主循环)
```

---

## 三、已解决的问题

### 问题 1: 编译错误 — `__packed` 指针与 `memcpy` 不兼容

**现象:** `app_coe.c:204/287` 报错 `#167: argument of type "__packed uint32_t *" is incompatible`

**原因:** `SDO_Header_t` 有 `__attribute__((packed))`，其成员 `data` 的类型为 `__packed uint32_t`，取地址后传给 `memcpy` 时 ARM Compiler 严格检查类型不匹配。

**解决:** 加显式强制转换
```c
memcpy((void *)&pSDO->data, pEntry->pData, pEntry->dataLen);       // Upload
memcpy(pEntry->pData, (const void *)&pSDO->data, dataLen);          // Download
```

### 问题 2: 编译警告 — `memset` 隐式声明

**现象:** `app_ethercat.c:65` 警告 `#223-D: function "memset" declared implicitly`

**原因:** `app_ethercat.c` 只 include 了 `app_ethercat.h`，后者 include `AX58100.h`，两个头文件都没有包含 `<string.h>`。

**解决:** 在 `app_ethercat.c` 和 `app_coe.c` 中显式 `#include <string.h>`，同时从 `app_coe.h` 中移除不必要的 `<string.h>` 引用。

### 问题 3: TwinCAT INIT→PREOP 超时 (ERR 状态)

**现象:** TwinCAT 报 `'INIT to PREOP' timeout (2000 ms) reached`，从站显示 ERR 状态。

**根因:** 上电后主站尚未写入 AL Control 时，ESC 的 AL Control 寄存器默认值为 `0x00`。`ECAT_MainTask` 读到 `requestedState=0`（无效状态），`IsValidTransition(1, 0)` 返回 false，从站将自身标为 Error 状态（AL Status 写 `0x21`）。

**解决:** 在 `ECAT_MainTask` 中增加两层防护：

```c
// 1. 过滤无效状态请求
if (requestedState != ESC_STATE_INIT  &&
    requestedState != ESC_STATE_PREOP &&
    requestedState != ESC_STATE_BOOT  &&
    requestedState != ESC_STATE_SAFEOP &&
    requestedState != ESC_STATE_OP)
{
    return m_currentState;  // 忽略, 不报错
}

// 2. 处理主站 Error ACK (bit5)
if (errAckBit && m_alError != AL_STATUS_NO_ERROR)
{
    m_alError = AL_STATUS_NO_ERROR;
    // 清除 AL Status Error 位
    ...
}
```

### 问题 4: TwinCAT 切换 PREOP 后 CoE Online 显示 offline

**现象:** 状态机能进入 PREOP/SAFEOP/OP，但 TwinCAT 的 CoE Online 面板显示 offline。

**分析:** 
- TwinCAT **需要有从站 ESI/XML 描述文件**才会知道从站支持 CoE，才会主动发起 SDO 请求读取对象字典
- 没有 ESI 文件时，TwinCAT 将设备识别为通用 Box，不会尝试 CoE 通信
- CoE 协议栈代码本身已经实现完毕（SDO Upload/Download），`g_dbg_coe_rxCnt` 和 `g_dbg_coe_procCnt` 可用于验证通信是否发生

**待解决:** 需要编写 AX58100 的 ESI/XML 从站描述文件，内容包括：
- 设备身份信息（厂商 ID、产品代码、版本号）
- SM0/SM1 邮箱配置（地址 0x1000/0x1080，128 字节）
- SM2/SM3 过程数据配置
- CoE 对象字典列表

---

## 四、调试变量速查表

Watch 窗口添加以下变量即可实时观察系统状态：

| 变量 | 含义 | 正常值 |
|------|------|--------|
| `g_dbg_callCnt` | ECAT_MainTask 调用次数 | 持续递增 |
| `g_dbg_alCtrlLo` | 主站请求的状态 (AL Control) | 0x01=INIT, 0x02=PREOP, 0x04=SAFEOP, 0x08=OP |
| `g_dbg_alStatus` | 从站回应的状态 (AL Status) | 与 alCtrlLo 镜像 ACK 位 |
| `ECAT_GetState()` | 软件当前状态 | 1/2/4/8 |
| `g_dbg_coe_rxCnt` | 收到邮箱数据次数 | >0 表示主站在发 SDO |
| `g_dbg_coe_procCnt` | 成功处理 SDO 次数 | >0 表示对象字典可读 |

---

## 五、下一步工作

1. **编写 ESI/XML 文件** — CoE 通信的前置条件
2. **验证 CoE 邮箱通信** — ESI 就绪后通过 `g_dbg_coe_rxCnt`/`g_dbg_coe_procCnt` 确认
3. **SDO 分段传输** — 当前 Download 仅支持加速传输（≤4 字节），需补充分段传输逻辑
4. **FMMU 配置验证** — SafeOp 阶段主站会配置 FMMU 映射
5. **分布式时钟 DC** — 同步精度需求时启用

---

## 六、关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 对象字典存储 | 静态数组 + 线性查找 | 嵌入式从站条目少（<50），O(n) 足够 |
| SM 初始化时机 | OnEnterState(PREOP) | 主站会在之后通过网络覆写，仅在无主站自测时有用 |
| 邮箱地址 | 硬编码 0x1000/0x1080 | 尝试动态读取导致 PREOP 跳转失败，回退到硬编码 |
| 状态过滤 | 白名单（5 个有效状态） | 比检查 `==0` 更健壮，覆盖所有非法值 |
| Download 长度 | `>` 而非 `!=` | 允许主站写入少于对象容量的数据 |
