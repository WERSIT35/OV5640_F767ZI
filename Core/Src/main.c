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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "camera.h"
#include "lcd.h"
#include "touch.h"
#include "app.h"
#include "ov5640.h"
#include "splash.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Red blink code for "HSE never became ready" - see Error_Handler.  Picks up
 * where the Camera_Status_t codes (2..9) leave off. */
#define DIAG_CODE_NO_HSE      10U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

DCMI_HandleTypeDef hdcmi;

I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */

/* Live-preview statistics, refreshed once per second, readable over SWD. */
typedef struct
{
  uint32_t camera_fps;        /* complete frames from the DCMI, per second  */
  uint32_t display_fps;       /* frames sent to the LCD, per second         */
  uint32_t overruns;          /* DCMI FIFO overruns since reset             */
  uint32_t short_frames;      /* frames that ended early                    */
  uint32_t dropped;           /* frames lost because the LCD was still busy */
  uint32_t restarts;          /* sensor restarts after a stall              */
} Preview_Stats_t;

volatile Preview_Stats_t preview_stats;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DCMI_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*
 * LEDs: LD1 green (PB0) toggles once a second while the preview runs.
 * LD3 red (PB14) blinks a Camera_Status_t code if the camera cannot start.
 * Both alternate in Error_Handler.  LD2 is on PB7, used for DCMI_VSYNC.
 */
static void Led_Init(void)
{
  GPIO_InitTypeDef led = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  led.Pin   = GPIO_PIN_0 | GPIO_PIN_14;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &led);
}

/** Flash the red LED `code` times, then hold dark for a beat. */
static void Diag_BlinkCode(uint32_t code)
{
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0,  GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
  HAL_Delay(700U);

  for (uint32_t i = 0U; i < code; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    HAL_Delay(250U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
    HAL_Delay(250U);
  }

  HAL_Delay(700U);
}

/** Busy-wait without SysTick, for fault paths with interrupts off. */
static void Diag_SpinMs(uint32_t ms)
{
  uint32_t n = (SystemCoreClock / 8000U) * ms;

  for (volatile uint32_t i = 0U; i < n; i++) { }
}

/** Start the sensor, showing a fault and retrying until it answers. */
static void Camera_Bringup(void)
{
  for (uint32_t tries = 0U; ; tries++)
  {
    Camera_Status_t status;

    /* A fresh I2C peripheral clears a bus state left by a half transfer. */
    if (tries > 0U)
    {
      (void)HAL_I2C_DeInit(&hi2c2);
      MX_I2C2_Init();
    }

    Lcd_WaitIdle();
    status = Camera_Init();

    if (status == CAMERA_OK)
    {
      return;
    }

    if (tries >= 2U)
    {
      App_ShowFault(ICON_CAMERA);
      Diag_BlinkCode((uint32_t)status);
    }
  }
}

/** Start the preview, restarting the sensor until frames flow. */
static void Camera_StartOrRetry(void)
{
  while (Camera_StartLive() != CAMERA_OK)
  {
    App_ShowFault(ICON_CAMERA);
    Diag_BlinkCode((uint32_t)CAMERA_ERR_TIMEOUT);
    Camera_Bringup();
    App_ApplySettings();
  }
}

/**
  * @brief  Turn on the I- and D-cache, keeping the frame buffers uncached.
  * @note   MPU region 1 covers 0x20000000-0x2007FFFF as normal, non-cacheable
  *         memory, with its first two 64K subregions disabled so the DTCM
  *         (0x20000000-0x2001FFFF) keeps the default map - the DTCM is never
  *         cached anyway.  What is left is SRAM1+SRAM2, which the linker
  *         script reserves for .framebuf.
  *
  *         Chosen over cache maintenance (invalidate after DCMI, clean before
  *         SPI DMA) because the CPU never reads or writes pixels on the live
  *         path: caching the buffers would buy nothing, and every missed or
  *         mis-sized maintenance call would show up as stale stripes.
  *         Everything else - code in flash, variables and stack in DTCM -
  *         gets the full benefit of the caches.
  *
  *         Region 0 is CubeMX's background region (MPU_Config); region 1 has
  *         the higher number, so it wins where they overlap.
  */
static void Cache_Config(void)
{
  MPU_Region_InitTypeDef region = {0};

  HAL_MPU_Disable();

  region.Enable           = MPU_REGION_ENABLE;
  region.Number           = MPU_REGION_NUMBER1;
  region.BaseAddress      = 0x20000000U;
  region.Size             = MPU_REGION_SIZE_512KB;
  region.SubRegionDisable = 0x03U;
  region.TypeExtField     = MPU_TEX_LEVEL1;
  region.AccessPermission = MPU_REGION_FULL_ACCESS;
  region.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  region.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  region.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  region.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&region);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  SCB_EnableICache();
  SCB_EnableDCache();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  Cache_Config();
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  Led_Init();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DCMI_Init();
  MX_I2C2_Init();
  MX_USART3_UART_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  /* The screen comes up first so the rest of the loading is visible. */
  Lcd_Init();
  Splash_Intro();

  Splash_Step("STARTING CAMERA", 30);
  Camera_Bringup();

  Splash_Step("LOADING SETTINGS", 70);
  App_Init();

  /* Calibrate on first boot (nothing saved yet), or when the shutter button
   * (PF14) is held while the board starts. */
  if (!Touch_Init() ||
      (HAL_GPIO_ReadPin(SHUTTER_GPIO_Port, SHUTTER_Pin) == GPIO_PIN_RESET))
  {
    Touch_Calibrate();
  }

  Splash_Step("READY", 100);
  HAL_Delay(250U);

  Camera_StartOrRetry();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /*
     * Live preview.  Each pass hands the newest camera frame to the LCD if
     * the LCD is free, and once a second publishes frame-rate statistics.
     */
    static bool     lcd_holds_frame = false;
    static uint32_t stats_start     = 0U;
    static uint32_t camera_base     = 0U;
    static uint32_t display_base    = 0U;
    static uint32_t stalled         = 0U;

    if (!Lcd_IsBusy())
    {
      int32_t index;

      if (lcd_holds_frame)
      {
        Camera_LiveRelease();
        lcd_holds_frame = false;
      }

      /* The bus is free between blits, so this is the only safe moment to
       * talk to the touch controller. */
      App_Poll();

      if (App_TakeCalibrationRequest())
      {
        Camera_StopLive();
        Touch_Calibrate();
        Camera_StartOrRetry();
        camera_base = Camera_GetLiveFrameCount();
        stats_start = HAL_GetTick();
      }

      /* App_Poll may have started the shutter flash, which owns the LCD. */
      if (!Lcd_IsBusy() && App_WantsFrame())
      {
        index = Camera_LiveAcquire();

        if (index >= 0)
        {
          uint16_t *frame = (uint16_t *)Camera_GetFrameBuffer((uint32_t)index);

          lcd_holds_frame = true;
          App_ProcessFrame(frame);
          Lcd_BlitAsync(frame);
        }
      }
    }

    if ((HAL_GetTick() - stats_start) >= 1000U)
    {
      uint32_t now_ms   = HAL_GetTick();
      uint32_t camera   = Camera_GetLiveFrameCount();
      uint32_t display  = Lcd_GetFrameCount();
      uint32_t interval = now_ms - stats_start;

      preview_stats.camera_fps   = ((camera - camera_base) * 1000U) / interval;
      preview_stats.display_fps  = ((display - display_base) * 1000U) / interval;
      preview_stats.overruns     = Camera_GetOverrunCount();
      preview_stats.short_frames = Camera_GetLiveShortCount();
      preview_stats.dropped      = Camera_GetLiveDroppedCount();

      App_SetFps(preview_stats.display_fps);

      /* A sensor that stops sending (brown-out, loose wire) is restarted. */
      stalled = (camera == camera_base) ? (stalled + 1U) : 0U;

      camera_base  = camera;
      display_base = display;
      stats_start  = now_ms;

      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);

      if (stalled >= 3U)
      {
        stalled = 0U;
        preview_stats.restarts++;
        Lcd_WaitIdle();
        Camera_StopLive();
        lcd_holds_frame = false;
        Camera_Bringup();
        App_ApplySettings();
        Camera_StartOrRetry();
        camera_base = Camera_GetLiveFrameCount();
        stats_start = HAL_GetTick();
      }
    }
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief DCMI Initialization Function
  * @param None
  * @retval None
  */
static void MX_DCMI_Init(void)
{

  /* USER CODE BEGIN DCMI_Init 0 */

  /* USER CODE END DCMI_Init 0 */

  /* USER CODE BEGIN DCMI_Init 1 */

  /* USER CODE END DCMI_Init 1 */
  hdcmi.Instance = DCMI;
  hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
  hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
  hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
  hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
  hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
  hdcmi.Init.ByteSelectMode = DCMI_BSM_ALL;
  hdcmi.Init.ByteSelectStart = DCMI_OEBS_ODD;
  hdcmi.Init.LineSelectMode = DCMI_LSM_ALL;
  hdcmi.Init.LineSelectStart = DCMI_OELS_ODD;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMI_Init 2 */
  /*
   * Polarities above match the OV5640's default DVP timing, not CubeMX's
   * defaults: the sensor changes data on the falling PCLK edge (so the DCMI
   * must sample on the rising edge) and pulses VSYNC high during vertical
   * blanking (so the "data invalid" level is HIGH).  These are set in the
   * .ioc as well, but check the DCMI tab after any pin change.
   *
   * HAL_DCMI_Init() enables the LINE and VSYNC interrupts, which would fire
   * 240 times per frame for no benefit here.  Keep only the error sources.
   */
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_LINE | DCMI_IT_VSYNC);
  /* USER CODE END DCMI_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x20404768;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 2000000;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOF, LCD_RST_Pin|T_CS_Pin|SD_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, LCD_DC_Pin|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin : PE2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : VBAT_SENSE_Pin */
  GPIO_InitStruct.Pin = VBAT_SENSE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(VBAT_SENSE_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_RST_Pin */
  GPIO_InitStruct.Pin = LCD_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(LCD_RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : T_CS_Pin SD_CS_Pin */
  GPIO_InitStruct.Pin = T_CS_Pin|SD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /*Configure GPIO pin : SHUTTER_Pin */
  GPIO_InitStruct.Pin = SHUTTER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(SHUTTER_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : T_IRQ_Pin */
  GPIO_InitStruct.Pin = T_IRQ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(T_IRQ_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LCD_CS_Pin LCD_DC_Pin */
  GPIO_InitStruct.Pin = LCD_CS_Pin|LCD_DC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : PD7 */
  GPIO_InitStruct.Pin = GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 10, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();

  /* SystemClock_Config runs before Led_Init, so a clock failure lands
   * here with the LED pins still unconfigured.  Initialising them again is
   * harmless if they already are. */
  Led_Init();

  /* HSE bypass relies on the ST-LINK's 8 MHz MCO.  If it was requested but
   * never arrived, say so specifically rather than with the generic pattern
   * below.  The PLL currently runs from HSI (this board's MCO does not reach
   * PH0), so HSEON is clear and this only fires if HSE is switched back on. */
  if (((RCC->CR & RCC_CR_HSEON) != 0U) && ((RCC->CR & RCC_CR_HSERDY) == 0U))
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_14, GPIO_PIN_RESET);

    for (;;)
    {
      for (uint32_t i = 0U; i < DIAG_CODE_NO_HSE; i++)
      {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
        Diag_SpinMs(250U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
        Diag_SpinMs(250U);
      }

      Diag_SpinMs(1400U);
    }
  }

  while (1)
  {
    /* Flash both LEDs together so reaching Error_Handler is distinguishable
     * from a hang.  Busy-wait rather than HAL_Delay: interrupts are off, so
     * SysTick never advances and HAL_Delay would block forever. */
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_14);

    Diag_SpinMs(200U);
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
