/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    spi.c
  * @brief   This file provides code for the configuration
  *          of the SPI instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "spi.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

SPI_HandleTypeDef hspi1;

/* SPI1 init function */
void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* 初始化 CS 为高电平 (不选中从设备)
   * PA4 在 MX_GPIO_Init 中默认设为低，需要修正 */
  SPI_CS_HIGH();

  /* USER CODE END SPI1_Init 2 */

}

void HAL_SPI_MspInit(SPI_HandleTypeDef* spiHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(spiHandle->Instance==SPI1)
  {
  /* USER CODE BEGIN SPI1_MspInit 0 */

  /* USER CODE END SPI1_MspInit 0 */
    /* SPI1 clock enable */
    __HAL_RCC_SPI1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**SPI1 GPIO Configuration
    PA5     ------> SPI1_SCK
    PA6     ------> SPI1_MISO
    PA7     ------> SPI1_MOSI
    */
    GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN SPI1_MspInit 1 */

  /* USER CODE END SPI1_MspInit 1 */
  }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef* spiHandle)
{

  if(spiHandle->Instance==SPI1)
  {
  /* USER CODE BEGIN SPI1_MspDeInit 0 */

  /* USER CODE END SPI1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SPI1_CLK_DISABLE();

    /**SPI1 GPIO Configuration
    PA5     ------> SPI1_SCK
    PA6     ------> SPI1_MISO
    PA7     ------> SPI1_MOSI
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7);

  /* USER CODE BEGIN SPI1_MspDeInit 1 */

  /* USER CODE END SPI1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/****************************************************************
 *                                                                *
 *     通    用    SPI    驱    动                                 *
 *                                                                *
 ****************************************************************/

#define SPI_TIMEOUT_MS      100U    /* 阻塞等待超时 */

/* ESC 测试结果 — 全局变量，方便调试时在 Watch 窗口直接观察 */
volatile uint8_t g_escType   = 0;    /* 0x0000: ESC 类型 */
volatile uint8_t g_escVer    = 0;    /* 0x0001: ESC 版本 */
volatile uint8_t g_escError  = 0;    /* 读写测试错误计数 */

/* ─────────────────────────────────────────────────────────────
 * 通用 SPI 基础收发 (含 CS 控制)
 * ───────────────────────────────────────────────────────────── */

HAL_StatusTypeDef SPI_WriteByte(uint8_t data)
{
    HAL_StatusTypeDef status;
    SPI_CS_LOW();
    status = HAL_SPI_Transmit(&hspi1, &data, 1, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

HAL_StatusTypeDef SPI_ReadByte(uint8_t *pData)
{
    HAL_StatusTypeDef status;
    uint8_t dummyTx = 0xFF;
    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, &dummyTx, pData, 1, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

HAL_StatusTypeDef SPI_WriteReadByte(uint8_t txData, uint8_t *pRxData)
{
    HAL_StatusTypeDef status;
    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, &txData, pRxData, 1, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

HAL_StatusTypeDef SPI_WriteBuffer(uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    SPI_CS_LOW();
    status = HAL_SPI_Transmit(&hspi1, pData, size, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

HAL_StatusTypeDef SPI_ReadBuffer(uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    uint16_t i;
    uint8_t dummyTx[128];

    /* 单次读取不能超过内部缓冲区大小 */
    if (size > 128) return HAL_ERROR;

    for (i = 0; i < size; i++) dummyTx[i] = 0xFF;
    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, dummyTx, pData, size, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

HAL_StatusTypeDef SPI_WriteReadBuffer(uint8_t *pTxData, uint8_t *pRxData, uint16_t size)
{
    HAL_StatusTypeDef status;
    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, pTxData, pRxData, size, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();
    return status;
}

/****************************************************************
 *                                                                *
 *     AX58100    ESC    PDI    专    用                            *
 *                                                                *
 *  协议: Beckhoff ESC SPI Slave (2 字节地址模式)                     *
 *  读:  [A[12:5]] [A[4:0]<<3|011] [0xFF 等待] [0xFF 数据...]       *
 *  写:  [A[12:5]] [A[4:0]<<3|100] [数据...]                        *
 *  MISO 地址段同时返回 [IRQ_0x0220] [IRQ_0x0221]                   *
 *                                                                *
 ****************************************************************/

static uint8_t g_esc_irq0 = 0;     /* 最近一次事务的 IRQ 状态 0x0220 */
static uint8_t g_esc_irq1 = 0;     /* 最近一次事务的 IRQ 状态 0x0221 */

/* ─────────────────────────────────────────────────────────────
 * 地址编码
 * ───────────────────────────────────────────────────────────── */

static void ESC_EncodeAddress(uint16_t addr, uint8_t cmd,
                               uint8_t *pAddr0, uint8_t *pAddr1)
{
    *pAddr0 = (uint8_t)((addr >> 5) & 0xFF);           /* A[12:5] */
    *pAddr1 = (uint8_t)(((addr & 0x1F) << 3) | (cmd & 0x07)); /* A[4:0]<<3 | CMD */
}

/* ─────────────────────────────────────────────────────────────
 * ESC 寄存器访问
 * ───────────────────────────────────────────────────────────── */

/**
 * @brief  ESC 读单个寄存器
 * @param  addr: 寄存器地址 (0x0000 ~ 0x1FFF)
 * @param  pData: 输出读取数据
 * @retval HAL_StatusTypeDef
 */
HAL_StatusTypeDef ESC_ReadRegister(uint16_t addr, uint8_t *pData)
{
    HAL_StatusTypeDef status;
    uint8_t txBuf[4];       /* addr0, addr1, wait(0xFF), read_term(0xFF) */
    uint8_t rxBuf[4];
    uint8_t addr0, addr1;

    ESC_EncodeAddress(addr, ESC_CMD_READ, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    txBuf[2] = 0xFF;        /* wait state: 等待 ESC 内部读取 */
    txBuf[3] = 0xFF;        /* read termination */

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 4, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];   /* 保存 IRQ 状态 */
        g_esc_irq1 = rxBuf[1];
        *pData = rxBuf[3];       /* 数据在第 4 个字节 */
    }
    return status;
}

/**
 * @brief  ESC 写单个寄存器
 * @param  addr: 寄存器地址 (0x0000 ~ 0x1FFF)
 * @param  data: 写入数据
 * @retval HAL_StatusTypeDef
 */
HAL_StatusTypeDef ESC_WriteRegister(uint16_t addr, uint8_t data)
{
    HAL_StatusTypeDef status;
    uint8_t txBuf[3];       /* addr0, addr1, data */
    uint8_t rxBuf[3];
    uint8_t addr0, addr1;

    ESC_EncodeAddress(addr, ESC_CMD_WRITE, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    txBuf[2] = data;

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 3, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
    }
    return status;
}

/**
 * @brief  ESC 读多个连续寄存器 (块读)
 * @note   地址从 addr 开始，每读一个字节地址自动递增
 * @param  addr: 起始寄存器地址
 * @param  pData: 接收缓冲区
 * @param  size: 读取字节数 (最大 ESC_MAX_BLOCK_SIZE)
 * @retval HAL_StatusTypeDef
 */
HAL_StatusTypeDef ESC_ReadBlock(uint16_t addr, uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    static uint8_t txBuf[2 + ESC_MAX_BLOCK_SIZE];  /* only addr + data, no wait byte */
    static uint8_t rxBuf[2 + ESC_MAX_BLOCK_SIZE];
    uint16_t i;
    uint8_t addr0, addr1;

    if (size == 0 || size > ESC_MAX_BLOCK_SIZE) return HAL_ERROR;

    ESC_EncodeAddress(addr, ESC_CMD_READ_NOWAIT, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    for (i = 0; i < size - 1; i++)
    {
        txBuf[2 + i] = 0x00;        /* 非终止字节: MOSI=0x00, ESC 继续预取 */
    }
    txBuf[2 + size - 1] = 0xFF;     /* 最后一个: MOSI=0xFF, 读终止 */

    SPI_CS_LOW();
    /* 短暂延时, 满足 ESC t_read ≥ 240ns (4.5MHz 下 3 个 NOP 远超此值) */
    __NOP(); __NOP(); __NOP();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2 + size, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
        for (i = 0; i < size; i++)
        {
            pData[i] = rxBuf[2 + i]; /* 数据从第 3 字节开始 (跳过地址段) */
        }
    }
    return status;
}

/**
 * @brief  ESC 写多个连续寄存器 (块写)
 * @param  addr: 起始寄存器地址
 * @param  pData: 发送数据缓冲区
 * @param  size: 写入字节数 (最大 ESC_MAX_BLOCK_SIZE)
 * @retval HAL_StatusTypeDef
 */
HAL_StatusTypeDef ESC_WriteBlock(uint16_t addr, uint8_t *pData, uint16_t size)
{
    HAL_StatusTypeDef status;
    static uint8_t txBuf[2 + ESC_MAX_BLOCK_SIZE];  /* 静态分配，避免栈溢出 */
    static uint8_t rxBuf[2 + ESC_MAX_BLOCK_SIZE];
    uint16_t i;
    uint8_t addr0, addr1;

    if (size > ESC_MAX_BLOCK_SIZE) return HAL_ERROR;

    ESC_EncodeAddress(addr, ESC_CMD_WRITE, &addr0, &addr1);
    txBuf[0] = addr0;
    txBuf[1] = addr1;
    for (i = 0; i < size; i++)
    {
        txBuf[2 + i] = pData[i];
    }

    SPI_CS_LOW();
    status = HAL_SPI_TransmitReceive(&hspi1, txBuf, rxBuf, 2 + size, SPI_TIMEOUT_MS);
    SPI_CS_HIGH();

    if (status == HAL_OK)
    {
        g_esc_irq0 = rxBuf[0];
        g_esc_irq1 = rxBuf[1];
    }
    return status;
}

/**
 * @brief  获取最近一次 SPI 事务的 ESC 中断请求状态
 * @note   ESC 在每次 SPI 地址段自动返回 AL Event Request 寄存器值
 * @param  irq0: AL Event Request [7:0]  (0x0220)
 * @param  irq1: AL Event Request [15:8] (0x0221)
 */
void ESC_GetIRQStatus(uint8_t *irq0, uint8_t *irq1)
{
    *irq0 = g_esc_irq0;
    *irq1 = g_esc_irq1;
}

/* ─────────────────────────────────────────────────────────────
 * 通用 SPI 测试
 * ───────────────────────────────────────────────────────────── */

void SPI_LoopbackTest(void)
{
    uint8_t txData, rxData;
    uint16_t errorCount = 0;
    HAL_StatusTypeDef status;

    for (txData = 0; txData < 0xFF; txData++)
    {
        rxData = 0x00;
        status = SPI_WriteReadByte(txData, &rxData);
        if (status != HAL_OK) { errorCount++; continue; }
        if (rxData != txData) { errorCount++; }
    }
    __NOP();    /* 断点: 检查 errorCount, 为 0 则通过 */
}

/**
 * @brief  SPI 发送测试数据模式 (用示波器观察)
 */
void SPI_SendTestPattern(void)
{
    uint8_t pattern[2] = {0x55, 0xAA};
    while (1)
    {
        SPI_WriteBuffer(pattern, 2);
        HAL_Delay(1);
    }
}

/* ─────────────────────────────────────────────────────────────
 * AX58100 ESC 测试
 * ───────────────────────────────────────────────────────────── */

void ESC_TestReadID(void)
{
    uint8_t irq0, irq1;

    /* 读 ESC Type (0x0000) */
    g_escType = 0;
    if (ESC_ReadRegister(0x0000, (uint8_t *)&g_escType) != HAL_OK)
    {
        g_escType = 0xFF;       /* 通信失败标记 */
    }

    /* 读 ESC Version (0x0001) */
    g_escVer = 0;
    if (ESC_ReadRegister(0x0001, (uint8_t *)&g_escVer) != HAL_OK)
    {
        g_escVer = 0xFF;        /* 通信失败标记 */
    }

    /* IRQ 状态 */
    ESC_GetIRQStatus(&irq0, &irq1);

    __NOP();    /* 在这里设断点，Watch 窗口观察 g_escType / g_escVer */
    (void)irq0;
    (void)irq1;
}

/**
 * @brief  ESC 测试: 读写用户 RAM 区域
 * @note   读写 ESC Process Data RAM (0x1000 起始)
 *         写入测试数据 → 读回 → 比较
 *         AX58100 有 9KB RAM, 用户可用区域从 0x1000 开始
 */
void ESC_TestReadWrite(void)
{
    #define TEST_SIZE   8
    uint8_t txData[TEST_SIZE] = {0xA5, 0x5A, 0x01, 0x02, 0x03, 0x04, 0x55, 0xAA};
    uint8_t rxData[TEST_SIZE];
    uint8_t i;

    g_escError = 0;

    /* 写测试数据到 RAM (0x1000) */
    if (ESC_WriteBlock(0x1000, txData, TEST_SIZE) == HAL_OK)
    {
        HAL_Delay(1);

        /* 读回 */
        if (ESC_ReadBlock(0x1000, rxData, TEST_SIZE) == HAL_OK)
        {
            for (i = 0; i < TEST_SIZE; i++)
            {
                if (rxData[i] != txData[i])
                {
                    g_escError++;
                }
            }
        }
        else
        {
            g_escError = 0xFF;      /* 读失败 */
        }
    }
    else
    {
        g_escError = 0xFE;          /* 写失败 */
    }

    __NOP();    /* 断点: g_escError==0 表示读写正确 */
}

/**
 * @brief  诊断函数: 一次性读出 ESC 关键信息, 排查问题
 */
void ESC_Diagnose(void)
{
    /* ---- 基本信息 ---- */
    volatile uint8_t type     = 0;    /* 0x0000: ESC Type */
    volatile uint8_t rev      = 0;    /* 0x0001: ESC Revision */
    volatile uint8_t fmmu     = 0;    /* 0x0004: FMMU 数量 */
    volatile uint8_t sm       = 0;    /* 0x0005: SyncManager 数量 */
    volatile uint8_t ramSize  = 0;    /* 0x0008: RAM 大小(KB) */

    /* ---- PDI 错误诊断 ---- */
    volatile uint8_t pdiErrCnt  = 0;  /* 0x030D: PDI 错误计数 */
    volatile uint8_t pdiErrCode = 0;  /* 0x030E: 最后错误原因 */

    /* ---- SM0 配置 (看看 0x1000 附近有没有被占用) ---- */
    volatile uint8_t sm0[8] = {0};    /* 0x0800~0x0807: SM0 配置 */

    /* ---- 0x1000 处现有数据 (不写入, 只读) ---- */
    volatile uint8_t ram[8] = {0};    /* 0x1000~0x1007: 原始数据 */

    /* ---- 用单字节读写 0x1F00 测试 (远离 SM 区域) ---- */
    volatile uint8_t testWr = 0xA5;
    volatile uint8_t testRd = 0;

    /* 读基本信息 */
    ESC_ReadRegister(0x0000, (uint8_t *)&type);
    ESC_ReadRegister(0x0001, (uint8_t *)&rev);
    ESC_ReadRegister(0x0004, (uint8_t *)&fmmu);
    ESC_ReadRegister(0x0005, (uint8_t *)&sm);
    ESC_ReadRegister(0x0008, (uint8_t *)&ramSize);

    /* 读 PDI 错误计数 */
    ESC_ReadRegister(0x030D, (uint8_t *)&pdiErrCnt);
    ESC_ReadRegister(0x030E, (uint8_t *)&pdiErrCode);

    /* 读 SM0 配置 */
    ESC_ReadBlock(0x0800, (uint8_t *)sm0, 8);

    /* 读 0x1000 处原始数据 */
    ESC_ReadBlock(0x1000, (uint8_t *)ram, 8);

    /* 单字节写 0x1F00 然后读回 (远离 SM 区域的高地址) */
    ESC_WriteRegister(0x1F00, 0xA5);
    ESC_ReadRegister (0x1F00, (uint8_t *)&testRd);

    __NOP();    /* 断点: 观察上面所有变量 */
    (void)type; (void)rev; (void)fmmu; (void)sm; (void)ramSize;
    (void)pdiErrCnt; (void)pdiErrCode;
    (void)sm0[0]; (void)ram[0]; (void)testWr; (void)testRd;
}

/* USER CODE END 1 */

