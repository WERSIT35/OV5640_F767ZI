# OV5640 camera on NUCLEO-F767ZI

Live camera preview on a 2.8" ILI9341 SPI touch display, with a touch
camera UI: colour effects, white balance, exposure compensation, night
mode, mirror/flip, grid, histogram, zebra, focus peaking, 2× zoom,
shutter with review, self-timer, battery and SD status icons.

- MCU: STM32F767ZI at 216 MHz (HSI → PLL), I/D-cache on
- Camera: OV5640, QVGA RGB565 over DCMI + DMA2, ~28 fps
- Display: ILI9341 320×240 over SPI1 at 54 MHz with DMA
- Touch: XPT2046 on the same SPI bus, calibration stored in flash

**Wiring, power, flashing and LED codes:** see [docs/WIRING.md](docs/WIRING.md).
**Current status and next steps:** see [docs/STATUS.md](docs/STATUS.md).

## Layout

| Path | What |
|---|---|
| `Core/Src/camera.c` | DCMI/DMA capture: snapshot and ping-pong live mode |
| `Core/Src/ov5640.c` | Sensor init and picture controls |
| `Core/Src/lcd.c` | ILI9341 driver, DMA blits |
| `Core/Src/touch.c` | XPT2046 driver and calibration |
| `Core/Src/app.c` | Camera UI: menu, status bar, shutter, review |
| `Core/Src/gfx.c`, `icons.c` | Drawing, font, icons, image tools |
| `Core/Src/power.c` | Battery gauge and die temperature (ADC1) |
| `Core/Src/sd.c` | SD card presence check |
| `Tools/` | PC scripts for the UART frame dump (debug mode) |

Custom code lives in its own files or in CubeMX `USER CODE` blocks, so
regenerating from `OV5640_F767ZI.ioc` keeps it.

Build with STM32CubeIDE (Debug configuration).
