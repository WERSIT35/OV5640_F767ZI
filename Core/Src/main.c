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
#include "ov5640.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

DCMI_HandleTypeDef hdcmi;

I2C_HandleTypeDef hi2c2;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DCMI_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==========================================================================
 * Bring-up diagnostics - remove once the UART is confirmed working.
 *
 * LD1 (PB0) and LD3 (PB14) are the only two Nucleo LEDs still free here;
 * LD2 is on PB7, which this project uses for DCMI_VSYNC.
 *
 * During start-up the markers latch, so a hang is readable long afterwards:
 *   both dark        -> not executing, or stuck in SystemClock_Config
 *   green on         -> clock/SysTick/GPIO fine, stuck in an MX_*_Init
 *   green + red on   -> all init done, but the USART never asserted TC
 *   green off, red on-> UART transmitted; stuck in Camera_Init
 *
 * Both markers are cleared once the capture loop starts, so a steady red
 * during normal running means something is wrong rather than nothing.
 *
 * After that the two LEDs have distinct jobs:
 *   green toggling   -> a frame was captured (heartbeat, one per frame)
 *   red blink codes  -> failure, flash count = Camera_Status_t value
 *   both alternating -> Error_Handler (an unexpected HAL failure)
 * ========================================================================== */

/* The I2C bus check, address scan and control-pin sweep add roughly three
 * seconds to every boot. Set to 0 once the camera is up. */
#define CAMERA_DIAG_VERBOSE   1

static void Diag_LedInit(void)
{
  GPIO_InitTypeDef led = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  led.Pin   = GPIO_PIN_0 | GPIO_PIN_14;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &led);
}

static void Diag_Blink(uint32_t times, uint32_t ms)
{
  for (uint32_t i = 0U; i < times; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_Delay(ms);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_Delay(ms);
  }
}

/**
  * @brief  Flash the red LED `code` times, then hold dark for a beat.
  * @note   Green is forced off first so the count is unambiguous.
  */
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

/** Repeat a blink code forever. Never returns. */
static void Diag_BlinkCodeForever(uint32_t code)
{
  for (;;)
  {
    Diag_BlinkCode(code);
  }
}

/**
  * @brief  Push a string straight into USART3's data register.
  * @note   No HAL, no newlib, no interrupts - if this produces nothing but
  *         the LEDs blink, the fault is in the UART peripheral, the board's
  *         VCP wiring, or the terminal, not in the application code.
  */
static bool Diag_Puts(const char *s)
{
  uint32_t guard;

  while (*s != '\0')
  {
    guard = 2000000U;
    while ((USART3->ISR & USART_ISR_TXE) == 0U)
    {
      if (--guard == 0U) { return false; }
    }

    USART3->TDR = (uint8_t)*s++;
  }

  guard = 2000000U;
  while ((USART3->ISR & USART_ISR_TC) == 0U)
  {
    if (--guard == 0U) { return false; }
  }

  return true;
}

/** Small blocking printf-alike onto USART3, for status messages only. */
static void Log(const char *fmt, ...)
{
  char    buf[128];
  va_list args;
  int     len;

  va_start(args, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len > 0)
  {
    if ((size_t)len > sizeof(buf) - 1U)
    {
      len = (int)sizeof(buf) - 1;
    }

    HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)len, 500U);
  }
}

#if CAMERA_DIAG_VERBOSE

/**
  * @brief  Report the idle levels of SDA/SCL, with and without the MCU's
  *         internal pull-ups.
  * @note   An I2C bus at rest must sit HIGH on both lines.  Reading LOW with
  *         no internal pull-up means either nothing is pulling the bus up
  *         (no resistors on the module) or something is holding it down
  *         (unpowered slave, short, stuck transaction).  Leaves the pins
  *         configured for the I2C peripheral with internal pull-ups on.
  */
static void Diag_I2cBusCheck(void)
{
  GPIO_InitTypeDef pin = {0};

  __HAL_RCC_GPIOF_CLK_ENABLE();

  pin.Pin   = GPIO_PIN_0 | GPIO_PIN_1;
  pin.Mode  = GPIO_MODE_INPUT;
  pin.Pull  = GPIO_NOPULL;
  pin.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &pin);
  HAL_Delay(2U);

  Log("I2C idle, floating:  SDA=%d SCL=%d\r\n",
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0),
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1));

  pin.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOF, &pin);
  HAL_Delay(2U);

  Log("I2C idle, pulled up: SDA=%d SCL=%d\r\n",
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0),
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1));

  /* Hand the pins back to I2C2, keeping the internal pull-ups engaged as a
   * fallback in case the module has no resistors of its own. They are weak
   * (~40k) but usually enough at 100 kHz over short wiring. */
  pin.Mode      = GPIO_MODE_AF_OD;
  pin.Pull      = GPIO_PULLUP;
  pin.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  pin.Alternate = GPIO_AF4_I2C2;
  HAL_GPIO_Init(GPIOF, &pin);

  /* Taking the pins away from the peripheral and giving them back can leave
   * the I2C state machine believing a transfer is in flight (BUSY latched),
   * which later surfaces as HAL_I2C_ERROR_TIMEOUT instead of a clean NACK.
   * Toggling PE resets the state machine. */
  __HAL_I2C_DISABLE(&hi2c2);
  HAL_Delay(2U);
  __HAL_I2C_ENABLE(&hi2c2);
}

/** Dump the I2C peripheral state so a stuck bus is visible directly. */
static void Diag_I2cRegs(const char *when)
{
  Log("I2C %s: CR1=%08lX ISR=%08lX (BUSY=%lu) err=%08lX\r\n",
      when,
      (unsigned long)I2C2->CR1,
      (unsigned long)I2C2->ISR,
      (unsigned long)((I2C2->ISR >> 15) & 1UL),
      (unsigned long)hi2c2.ErrorCode);
}

/**
  * @brief  Try the chip-ID read under all four PWDN/RESET pin combinations.
  * @note   Resolves the PE2/PD7 mapping empirically instead of trusting the
  *         assumption baked into ov5640_conf.h.  If the two are swapped, the
  *         normal power-up sequence leaves the sensor held in reset AND
  *         powered down at once, which looks exactly like a dead sensor.
  */
static void Diag_CameraPinSweep(void)
{
  Log("Control-pin sweep:\r\n");

  for (int e = 0; e <= 1; e++)
  {
    for (int d = 0; d <= 1; d++)
    {
      uint16_t          id = 0U;
      HAL_StatusTypeDef st;

      HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2,
                        e ? GPIO_PIN_SET : GPIO_PIN_RESET);
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7,
                        d ? GPIO_PIN_SET : GPIO_PIN_RESET);

      /* Generous settle time: the sensor needs a few ms of XCLK after
       * reset is released before its SCCB block will answer. */
      HAL_Delay(100U);

      hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
      st = OV5640_ReadChipId(&id);

      Log("  PE2=%d PD7=%d -> %s id=0x%04X err=%08lX\r\n",
          e, d, (st == HAL_OK) ? "ACK " : "NACK",
          (unsigned)id, (unsigned long)hi2c2.ErrorCode);
    }
  }

  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
}

/** Sweep every 7-bit address and report which ones acknowledge. */
static void Diag_I2cScan(void)
{
  uint32_t found = 0U;

  Log("I2C scan:");

  for (uint8_t addr = 0x08U; addr <= 0x77U; addr++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(addr << 1), 2U, 20U) == HAL_OK)
    {
      Log(" 0x%02X", (unsigned)addr);
      found++;
    }
  }

  if (found == 0U)
  {
    Log(" nothing responded");
  }

  Log("\r\n");
}

#endif /* CAMERA_DIAG_VERBOSE */

static const char *Camera_StatusName(Camera_Status_t status)
{
  switch (status)
  {
    case CAMERA_OK:            return "OK";
    case CAMERA_ERR_I2C:       return "I2C (no ACK from the sensor)";
    case CAMERA_ERR_CHIP_ID:   return "CHIP_ID (answered, but not 0x5640)";
    case CAMERA_ERR_CONFIG:    return "CONFIG (register write failed)";
    case CAMERA_ERR_DMA:       return "DMA";
    case CAMERA_ERR_TIMEOUT:   return "TIMEOUT (no end-of-frame)";
    case CAMERA_ERR_OVERRUN:   return "OVERRUN (PIXCLK too fast for HCLK)";
    case CAMERA_ERR_SYNC:      return "SYNC";
    case CAMERA_ERR_PARTIAL:   return "PARTIAL (armed mid-frame)";
    default:                   return "?";
  }
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

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Marker 1: clock, SysTick and GPIO are alive. Green latches on. */
  Diag_LedInit();
  Diag_Blink(3U, 150U);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DCMI_Init();
  MX_I2C2_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  /* Marker 2: every MX_*_Init returned. Red latches on. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);

  /* Raw first, so a failure here rules newlib out as the cause. */
  {
    bool uart_ok = Diag_Puts("\r\nRAW UART OK\r\n");

    /* Marker 3: green goes OUT once the UART has actually shifted those
     * bytes out (TC set).  Green staying ON means TXE/TC never asserted,
     * i.e. the USART itself is dead rather than the link to the PC. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0,
                      uart_ok ? GPIO_PIN_RESET : GPIO_PIN_SET);
  }

  Log("\r\n=== OV5640 / DCMI capture ===\r\n");
  Log("HCLK %lu Hz -> max DCMI PIXCLK ~%lu Hz\r\n",
      (unsigned long)HAL_RCC_GetHCLKFreq(),
      (unsigned long)((HAL_RCC_GetHCLKFreq() * 2UL) / 5UL));

#if CAMERA_DIAG_VERBOSE
  Diag_I2cRegs("before   ");
  Diag_I2cBusCheck();
  Diag_I2cRegs("after PE ");

  /* Clear accumulated errors so each stage's code is attributable - HAL ORs
   * into ErrorCode and never clears it by itself. */
  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
  Diag_I2cScan();
  Diag_I2cRegs("after scan");
  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;

  /* Get XCLK running before probing: without it the sensor cannot answer
   * SCCB at all, and the pin sweep below would be meaningless. */
  Camera_EnableXclk();
  Diag_CameraPinSweep();
#endif

  {
    Camera_Status_t status = Camera_Init();

    if (status != CAMERA_OK)
    {
      Log("Camera_Init failed: %s (chip ID read back as 0x%04X)\r\n",
          Camera_StatusName(status), (unsigned)OV5640_GetLastChipId());
      Log("I2C ErrorCode 0x%08lX\r\n", (unsigned long)hi2c2.ErrorCode);

      /* Blink the failure out rather than halting silently: the enum value
       * is the flash count. See Camera_Status_t in camera.h. */
      Diag_BlinkCodeForever((uint32_t)status);
    }
  }

  Log("OV5640 ready: %ux%u RGB565, %lu bytes/frame\r\n",
      (unsigned)OV5640_FRAME_WIDTH, (unsigned)OV5640_FRAME_HEIGHT,
      (unsigned long)Camera_GetBufferSize());
  Log("send 'c' to receive one frame\r\n");

  /* Bring-up is over, so drop the init marker.  From here the LEDs report
   * running state: green toggles per captured frame, red means trouble. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /*
     * Frames go out only when the host asks for one, by sending 'c'.
     *
     * Streaming them continuously made the UART useless for anything else:
     * the log text was buried in pixel data, and the receiver had to search
     * for the frame header inside a binary stream where the four-byte magic
     * collides constantly with ordinary pixel values.  On demand, the port
     * is readable text right up until the moment a frame is requested.
     */
    static bool     trigger_seen = false;
    static uint32_t since_dump   = 0U;

    uint8_t cmd       = 0U;
    bool    want_dump = (HAL_UART_Receive(&huart3, &cmd, 1U, 5U) == HAL_OK)
                        && ((cmd == 'c') || (cmd == 'C'));

    if (want_dump)
    {
      trigger_seen = true;
    }
    else if (!trigger_seen && (++since_dump >= 8U))
    {
      /* No trigger has ever arrived, so RX is probably not wired up (only
       * PD8 is needed to listen).  Fall back to sending a frame every so
       * often, which keeps the port usable without a second wire. */
      since_dump = 0U;
      want_dump  = true;
    }

    Log("capture...\r\n");

    Camera_Status_t status = Camera_Capture(3000U);

    Log("  -> %s after %lu attempt(s), %lu/%lu words left\r\n",
        Camera_StatusName(status),
        (unsigned long)Camera_GetLastAttempts(),
        (unsigned long)Camera_GetTimeoutRemainingWords(),
        (unsigned long)Camera_GetExpectedWords());

    if (status == CAMERA_OK)
    {
      /* Heartbeat: green toggles once per captured frame. */
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);

      if (Camera_LastFrameHadOverrun())
      {
        Log("frame ok but overran (%lu total) - may show a tear; "
            "raise OV5640_PLL_SYSDIV\r\n",
            (unsigned long)Camera_GetOverrunCount());
      }

      if (want_dump)
      {
        Camera_DumpFrameUart(&huart3);
      }
    }
    else
    {
      Log("capture failed: %s (overruns %lu, DMA %lu/%lu words left)\r\n",
          Camera_StatusName(status),
          (unsigned long)Camera_GetOverrunCount(),
          (unsigned long)Camera_GetTimeoutRemainingWords(),
          (unsigned long)Camera_GetExpectedWords());

      {
        uint32_t cr, sr, risr, misr, frames, vsyncs;

        Camera_GetDebugRegs(&cr, &sr, &risr, &misr, &frames, &vsyncs);
        Log("  DCMI CR=%08lX SR=%08lX RISR=%08lX MISR=%08lX\r\n",
            (unsigned long)cr, (unsigned long)sr,
            (unsigned long)risr, (unsigned long)misr);
        Log("  IRQ counts: frame=%lu vsync=%lu, attempts=%lu\r\n",
            (unsigned long)frames, (unsigned long)vsyncs,
            (unsigned long)Camera_GetLastAttempts());
      }

      /* One red pulse per failure. The full blink code is reserved for
       * Camera_Init, where there is no working UART to fall back on -
       * here the log line above says far more, far faster. */
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
      HAL_Delay(60U);
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
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
  hi2c2.Init.Timing = 0x00303D5B;
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
  huart3.Init.BaudRate = 115200;
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
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin : PE2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PD7 */
  GPIO_InitStruct.Pin = GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

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
  while (1)
  {
    /* Flash both LEDs together so reaching Error_Handler is distinguishable
     * from a hang.  Busy-wait rather than HAL_Delay: interrupts are off, so
     * SysTick never advances and HAL_Delay would block forever. */
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_14);

    for (volatile uint32_t i = 0U; i < 400000U; i++) { }
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
