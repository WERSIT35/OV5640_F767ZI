# Project status – checkpoint 2026-11-03

## Firmware: done and on `main`

| Area | State |
|---|---|
| Clocks | HSI → PLL 216 MHz, overdrive, I/D-cache on (HSE bypass unavailable: no ST-LINK MCO) |
| Camera | OV5640 QVGA RGB565, DCMI continuous + ping-pong buffers, ~28 fps at sysdiv 2, 0 overruns |
| Display | ILI9341 over SPI1 at 54 MHz with DMA, 43.9 fps full-screen fill |
| Touch | XPT2046, 3-point calibration saved in flash sector 11 |
| UI | Icon status bar and menu: effects, WB, EV, night, orientation, grid, histogram, zebra, peaking, 2x zoom, shutter + review, self-timer, draw mode |
| Status icons | Battery % (PA3 divider), SD presence (CS on PF15), overheat, external power |
| Fault display | Struck-out camera icon + LED blink code if the sensor fails |

Last measured on hardware: camera 27–28 fps, display 27–28 fps, no
overruns or drops, touch working, calibration stored.

## Hardware: open issue

**The STM32 runs very hot when the camera/display/ST-LINK are wired up.**

What is known:

- The chip reported its own supply as ~2.7 V (expected 3.3 V) and its die
  as ~87 °C – consistent with current being forced into it.
- **Erased chip, nothing connected, JP5 in: stays cool.** So the STM32 is
  not shorted internally and the firmware is not the cause.
- JP5 was in place throughout, so it is not the cause either.
- The chip is currently **fully erased** (flash and calibration sector
  blank).

## Next steps

1. Reconnect one part at a time, chip still erased, power on 2–3 s each
   and feel the STM32. Stop at the first step that heats it:
   1. ST-LINK (5 SWD wires, see WIRING.md §2)
   2. Display – VCC/GND/LED, then signals
   3. Camera without XCLK
   4. Camera XCLK (PA8) alone – if this one, the module has its own
      oscillator: leave XCLK off and set `OV5640_DRIVE_XCLK_FROM_MCO1` to 0
   5. Shutter button
2. Fix the wire or module that brings the heat back.
3. With everything connected and cool, reflash:
   `STM32_Programmer_CLI -c port=SWD mode=UR -w Debug/OV5640_F767ZI.elf -v -rst`
   (rebuild in STM32CubeIDE first if `Debug/` is missing – it is not
   committed).
4. Check the chip stays cool while the firmware runs, then re-run the
   touch calibration (it was erased).
5. Measure 3V3 under load; it should be ~3.3 V.
