# OV5640 camera on NUCLEO-F767ZI

Live camera preview on a 2.8" ILI9341 SPI touch display, with a start-up
animation and a tabbed touch UI: picture styles, colour effects, white
balance, exposure compensation, night mode, 1x/2x/4x sensor zoom, rotation,
grid, histogram, zebra, focus peaking, shutter with review, self-timer,
battery and SD status. Settings are kept in flash. Photos are not saved yet.

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
| `Core/Src/settings.c` | Saved settings in flash sector 10 |
| `Core/Src/splash.c` | Start-up animation |
| `Core/Src/sd.c` | SD card presence check |
| `Tools/` | Old PC scripts for the UART frame dump, which the firmware no longer has |

Custom code lives in its own files or in CubeMX `USER CODE` blocks, so
regenerating from `OV5640_F767ZI.ioc` keeps it.

Build with STM32CubeIDE (Debug configuration).
