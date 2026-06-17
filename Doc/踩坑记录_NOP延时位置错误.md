# 踩坑记录：SPI 块读中 NOP 延时位置错误

> 面试可讲 · 嵌入式 C / SPI 驱动 / EtherCAT ESC 开发实战经验

---

## 背景

芯片：**AX58100**（EtherCAT 从站控制器 ESC），通过 **SPI** 与 **STM32F103**（72MHz）通信。

ESC 的 SPI PDI 支持两种读命令：

| 命令 | 编码 | 特点 |
|------|------|------|
| CMD=0x03 | Read + Wait State | 地址段后有一个 wait state 字节（8 个 SPI 时钟），硬件级等待 |
| CMD=0x02 | Read No Wait | 无 wait state 字节，地址后紧接数据，**需要软件保证延时** |

---

## 踩坑：NOP 放错了位置

### 原始代码（有问题）

```c
/* 块读函数 ESC_ReadBlock 中的时序 */
SPI_CS_LOW();
__NOP(); __NOP(); __NOP();                           // ← NOP 在这里
status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2 + size, SPI_TIMEOUT_MS);
SPI_CS_HIGH();
```

波形示意：

```
CS  ─┐                                              ┌──
     └── NOP×3 ── [addr0] [addr1] [d0] [d1]...[dN] ──┘
         ↑                       ↑
    NOP 在这里               t_read 需要延时在这里
    （对 t_read 无用）       （但字节间没有停顿）
```

CMD=0x03 的 wait state 字节（8 个 SPI 时钟 ≈ 1.78μs）插在**地址和数据之间**，给 ESC 时间取数据。CMD=0x02 去掉了这个字节，本来应该在**同样位置**用软件延时补上，但 NOP 被错误地放在了 CS 拉低之后、SPI 时钟开始之前——完全不在 t_read 的路径上。

### 为什么 4.5MHz 下居然能工作

```
地址段 2 字节传输时间 = 16 bit × (1/4.5MHz) ≈ 3.56μs
ESC t_read 硬件要求    = 0.24μs
裕量                  = 3.56μs - 0.24μs = 3.32μs（14 倍）
```

SPI 频率低，地址传输本身已经远超 240ns，ESC 在收地址的同时就完成了数据准备。**纯靠运气**，NOP 没有起作用。

如果 SPI 频率提高到 30MHz：
```
地址段 = 16 × 33ns = 533ns
裕量   = 533ns - 240ns = 293ns  ← 开始吃紧，再加中断抖动就可能出错
```

### 可以怎么发现问题

| 方法 | 操作 | 判据 |
|------|------|------|
| 注释 NOP 压测 | `ESC_TestReadWrite` 循环 10000 次 | `g_escError` 始终为 0 → NOP 确实没有起作用 |
| 读 PDI Error Counter | 读 ESC 寄存器 `0x030D` | >0 → ESC 硬件检测到 SPI 通信错误 |
| 逻辑分析仪 | 抓 SCK / MOSI / CS 波形 | 测量地址最后一 bit 到数据第一 bit 的间隔 |

---

## 修复：拆为两次 SPI 传输

### 修复后代码

```c
SPI_CS_LOW();

/* 第一步: 只发地址段 (2 字节) */
status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2, SPI_TIMEOUT_MS);

/* ⚡ 地址和数据之间的延时: ESC 趁这个间隙取数据 */
__NOP(); __NOP(); __NOP(); __NOP(); __NOP();
__NOP(); __NOP(); __NOP(); __NOP(); __NOP();

/* 第二步: 发数据段 (size 字节)，ESC 已经有数据等着了 */
if (status == HAL_OK)
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf + 2, rxBuf + 2, size, SPI_TIMEOUT_MS);

SPI_CS_HIGH();
```

### 修复后波形

```
CS  ─┐                                                          ┌──
     └──[addr0][addr1]── NOP×10 + HAL开销 ──[d0][d1]...[dN]──┘
                        ↑
                  t_read 延时在正确的位置
                  （SPI 时钟停止，ESC 取数据）
```

### 关键设计决策

1. **CS 全程拉低**：ESC 认为这是一次完整事务，不会中途重置状态机
2. **两次 HAL 调用**：拆开后可以精确控制地址和数据之间的延时
3. **10 个 NOP**：保守值，加上 HAL 函数返回开销和下一次调用准备，总延时远超 240ns，且不随 SPI 频率变化
4. **rxBuf 分两次填充**：第一次收 IRQ 状态（byte 0-1），第二次收数据（byte 2-N），不影响原有数据提取逻辑

---

## 经验总结

### 技术层面

1. **延时要放在"被等待的事件之后、依赖的事件之前"**，不是随便找个 CS LOW 之后就能起作用的
2. **一个 HAL 调用内部是连续 SPI 时钟**，字节间间隙由硬件控制，软件插不进去 NOP——要控制时序就必须拆成多次调用
3. **低速下"能跑"不代表"写得对"**，用逻辑分析仪抓波形，或者提高频率压测才能暴露时序问题
4. **ESC 的 PDI Error Counter（0x030D）是硬件级别的检错**，出现问题先读它，不要只看软件侧的数据比对

### 面试话术

> "在做 EtherCAT 从站驱动时，我发现块读的 NOP 延时放错了位置——放在 CS 低电平之后而不是地址和数据之间，对 t_read 时序完全没有作用。之所以 4.5MHz 下能工作，是因为地址段传输本身耗时 3.56μs，远超 ESC 要求的 240ns。我通过将一次 SPI 传输拆为两次 HAL 调用、把 NOP 夹在中间，修复了这个潜在的时序隐患。这个 bug 在低速下不暴露，一旦把 SPI 提到 30MHz 以上就会随机出错，属于典型的'能跑但不对'的隐患。"

---

## 相关文件

- `Core/Src/spi.c` — `ESC_ReadBlock()` 函数（修复位置）
- `SPI修改日志.md` — 完整的开发调试记录
- `ESC寄存器与SPI通信FAQ.md` — ESC SPI 通信协议参考

---

> 最后更新：2026-06-14
> 状态：已修复验证通过，待高速 SPI 下压测确认
