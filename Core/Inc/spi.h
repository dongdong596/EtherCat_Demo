/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    spi.h
  * @brief   This file contains all the function prototypes for
  *          the spi.c file
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
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SPI_H__
#define __SPI_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern SPI_HandleTypeDef hspi1;

/* USER CODE BEGIN Private defines */

/* ================================================================
 * 通用 SPI 驱动
 * ================================================================ */

/* 软件 CS 片选控制
 * 当前只用了 PA4 一根 CS，接 AX58100 的 SCS_ESC (PDI 接口)
 * SPI_CS_HIGH 在拉高 CS 之前会等待 SPI 移位寄存器完全空闲 (BSY=0)
 * 否则 AX58100 会报 "Read continued after termination" 错误 (0x030E bit5)
 */
#define SPI_CS_PORT             GPIOA
#define SPI_CS_PIN              GPIO_PIN_4
#define SPI_CS_LOW()            HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_RESET)
#define SPI_CS_HIGH()           do { \
                                    while (__HAL_SPI_GET_FLAG(&hspi1, SPI_FLAG_BSY)) {} \
                                    HAL_GPIO_WritePin(SPI_CS_PORT, SPI_CS_PIN, GPIO_PIN_SET); \
                                } while(0)


/* USER CODE END Private defines */

void MX_SPI1_Init(void);

/* USER CODE BEGIN Prototypes */

/* ================================================================
 * 通用 SPI 驱动
 *=============================================================== */

/* 基础收发 (含 CS 控制) */
HAL_StatusTypeDef SPI_WriteByte(uint8_t data);
HAL_StatusTypeDef SPI_ReadByte(uint8_t *pData);
HAL_StatusTypeDef SPI_WriteReadByte(uint8_t txData, uint8_t *pRxData);
HAL_StatusTypeDef SPI_WriteBuffer(uint8_t *pData, uint16_t size);
HAL_StatusTypeDef SPI_ReadBuffer(uint8_t *pData, uint16_t size);
HAL_StatusTypeDef SPI_WriteReadBuffer(uint8_t *pTxData, uint8_t *pRxData, uint16_t size);

/* 通用测试 */
void SPI_LoopbackTest(void);       /* 自环回: 短接 MOSI/MISO 验证底层硬件 */
void SPI_SendTestPattern(void);    /* 波形测试: 0x55/0xAA 交替, 示波器观察 */


/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H__ */

