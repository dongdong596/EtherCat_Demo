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

/* USER CODE END 1 */

