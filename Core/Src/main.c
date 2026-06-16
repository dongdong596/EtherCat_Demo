/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "AX58100.h"
#include "app_ethercat.h"
#include "app_coe.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */

  /* 上电后读取 ESC 完整信息, 结果存入 g_escInfo 结构体
   * 设断点在下一行, Watch 窗口展开 g_escInfo 查看所有字段 */
  AX58100_ReadESCInfo();

  /* 初始化 EtherCAT 状态机, 写 AL Status = Init */
  ECAT_Init();

  /* 初始化 CoE 协议栈 (对象字典) */
  CoE_Init();

  /* 禁用 ESC 看门狗, 防止开发阶段异常复位 */
  ESC_Watchdog_Config();

  __NOP();    /* 断点: 观察 g_escInfo + ECAT_GetState() */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /* ============================================================
     * AX58100 ESC 测试菜单
     *
     * [CORE] ECAT_MainTask():       (第4步) 状态机循环 — 正常运行时默认
     *
     * [T]  ECAT_SelfTest():         (第4步自测) 不需网线, 返回 0 = 通过
     *                              手动模拟 Init→PreOp→SafeOp→Op→Init 全流程
     * [1] AX58100_ReadESCInfo():   (第3步) 块读 ESC 完整信息 → g_escInfo
     * [2] ESC_TestReadID():        验证通信, 读 Type/Version
     * [3] ESC_TestReadWrite():     读写用户 RAM, 验证 PDI 功能
     * [4] ESC_Diagnose():          全诊断: 身份+PDI+SM0+RAM
     * [5] SPI_LoopbackTest():      自环回 - 需短接 MISO/MOSI
     * [6] SPI_SendTestPattern():   波形测试 - 示波器观察
     * ============================================================ */

    /* ---- 状态机 (正常运行时启用) ---- */
    ECAT_MainTask();

    /* ---- CoE 邮箱通信 (PREOP/SAFEOP/OP 态处理 SDO) ---- */
    CoE_MainTask();

    /* ---- 过程数据交换 (OP 态下激活) ---- */
    ECAT_ProcessDataExchange();

    /* ---- 测试模式: 取消注释下面其中一行 ---- */
    // ECAT_SelfTest();              /* [T] 第4步自测: 返回 0 即通过 */
    // AX58100_ReadESCInfo();      /* [1] 第3步: ESC 完整信息    */
    // ESC_TestReadID();           /* [2] 读 ESC 类型/版本       */
    // ESC_TestReadWrite();        /* [3] 读写用户 RAM           */
    // ESC_Diagnose();             /* [4] 全诊断                 */
    // SPI_LoopbackTest();         /* [5] 自环回                 */
    // SPI_SendTestPattern();      /* [6] 发送波形               */

    HAL_Delay(10);                /* 状态机 10ms 周期 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
