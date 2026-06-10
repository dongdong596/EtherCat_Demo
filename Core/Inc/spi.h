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

/* ----------------------------------------------------------------
 * AX58100 ESC PDI 专用
 * ----------------------------------------------------------------
 *
 * 命令码 (Table 6-2: SPI commands CMD0 and CMD1)
 *   010 = Read
 *   011 = Read with Wait State (推荐, 自动插入 0xFF 等待字节)
 *   100 = Write
 *   110 = Address Extension (3 字节地址模式)
 *
 * SCS_ESC ── PA4     (PDI SPI, 访问 ESC 寄存器/内存)
 * SCS_FUNC ── 未配置  (Function SPI, 访问 AX58100 外设寄存器)
 * SCK      ── PA5
 * MISO     ── PA6
 * MOSI     ── PA7
 * 通信模式: SPI Mode 3 (CPOL=1, CPHA=1), MSB first, 4.5 MHz
 */
#define ESC_CMD_READ            0x03U   /* 模式1: Read+Wait State (保留) */
#define ESC_CMD_READ_NOWAIT     0x02U   /* 模式2: Read 无等待 (推荐块读用) */
#define ESC_CMD_WRITE           0x04U
#define ESC_MAX_BLOCK_SIZE      128U

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

/* ----------------------------------------------------------------
 * AX58100 ESC PDI 专用
 * ---------------------------------------------------------------- */

/* ESC 寄存器访问 (2 字节地址模式, 带等待状态) */
HAL_StatusTypeDef ESC_ReadRegister(uint16_t addr, uint8_t *pData);
HAL_StatusTypeDef ESC_WriteRegister(uint16_t addr, uint8_t data);
HAL_StatusTypeDef ESC_ReadBlock(uint16_t addr, uint8_t *pData, uint16_t size);
HAL_StatusTypeDef ESC_WriteBlock(uint16_t addr, uint8_t *pData, uint16_t size);
void              ESC_GetIRQStatus(uint8_t *irq0, uint8_t *irq1);

/* ESC 测试 */
void ESC_TestReadID(void);         /* 读 ESC Type/Version 寄存器, 验证通信 */
void ESC_TestReadWrite(void);      /* 读写用户 RAM 区域, 验证 PDI 功能 */
void ESC_Diagnose(void);           /* 诊断: 读 PDI 错误/ SM 配置 / RAM 前几个字节 */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H__ */

