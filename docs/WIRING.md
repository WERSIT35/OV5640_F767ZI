# Wiring guide

Everything needed to wire the OV5640 camera, the 2.8" ILI9341 touch display,
the buttons, power and the debugger to a **NUCLEO-F767ZI**.

Arduino-style labels (D2, D23, A0 …) are printed on the Nucleo next to the
Zio headers. Pins with no such label are on the long two-row **morpho**
headers along the edges (CN11 on the left, CN12 on the right); their names
are printed on the underside of the board.

> **Read the power section first.** A swapped GND/3V3 wire on this build
> damaged the STM32 (it ran very hot from then on). Check polarity with a
> meter before the first power-up of any new wiring.

---

## 1. Power

| From | To | Notes |
|---|---|---|
| 5 V supply **+** | slide switch, middle pin | Supply must be 4.8–5.2 V, ≥ 1 A |
| slide switch, outer pin | Nucleo **5V** pin (power header CN8) | The other outer pin stays unconnected |
| 5 V supply **−** | Nucleo **GND** | Never switched |
| Nucleo **3V3** | camera 3V3, display LED | |
| Nucleo **5V** | display VCC | The module has its own 3.3 V regulator; leave its J1 pads open |
| Nucleo **GND** | camera GND, display GND | Use a separate GND pin for each if you can |

Rules:

- Feed 5 V into the **5V** pin (or **E5V**, CN11 pin 6, with the power
  jumper moved to E5V). **Never into VIN** – VIN expects 7–12 V and drops
  5 V to about 3.5 V, and the board then browns out whenever the camera
  starts.
- **Never feed 3.3 V or 5 V from a USB-UART adapter** into the board. Its
  3.3 V pin is ~100 mA and its 5 V passes through a small fuse; both
  collapse under the camera + display load (~350–450 mA).
- **Never power anything from the ST-LINK.** It only debugs.
- With the ST-LINK snapped off, remove jumper **JP5** if plugging the
  ST-LINK USB in makes the board lose power.

## 2. ST-LINK (SWD programming and debugging)

The ST-LINK is a separate, snapped-off ST-LINK/V2-1 connected to the
Nucleo by wires. Its 6-pin **SWD header (CN6)** is pin 1 at the end marked
with a dot, square pad or "1".

| ST-LINK CN6 pin | Signal | Nucleo pin | Where |
|---|---|---|---|
| 1 | VDD_TARGET (sense only) | **3V3** | power header CN8 |
| 2 | SWCLK | **PA14** | morpho CN11 |
| 3 | GND | **GND** | any GND |
| 4 | SWDIO | **PA13** | morpho CN11 |
| 5 | NRST | **RESET** (NRST) | power header CN8 |
| 6 | SWO | **not connected** | PB3 is the display SCK |

On a Nucleo-144, PA13 and PA14 are near the top of CN11 (normally pins 13
and 15, with RESET on 14 and 3V3 on 16) – confirm against the labels on the
underside before connecting.

If the ST-LINK piece has a two-jumper header (ST-LINK / NUCLEO), remove
both jumpers so its SWD signals go to CN6.

The voltage the ST-LINK reports for this setup is always ~0.03 V; ignore
it. A reading of "No STM32 target found" almost always means the Nucleo has
no power.

## 3. OV5640 camera (DCMI + SCCB) – fixed, do not change

| Camera pin | STM32 | Nucleo label |
|---|---|---|
| 3V3 | 3V3 | 3V3 |
| GND | GND | GND |
| SIOD (SDA) | PF0 | D68 |
| SIOC (SCL) | PF1 | D69 |
| XCLK | PA8 | morpho CN12, "PA8" |
| PWDN | PD7 | D51 |
| RESET | PE2 | D31 |
| PCLK | PA6 | D12 |
| HREF / HSYNC | PA4 | D24 |
| VSYNC | PB7 | morpho only (shared with LED LD2) |
| D0 | PC6 | D16 |
| D1 | PC7 | D21 |
| D2 | PC8 | D43 |
| D3 | PC9 | D44 |
| D4 | PC11 | D46 |
| D5 | PB6 | D26 |
| D6 | PB8 | D15 |
| D7 | PB9 | D14 |

Modules that label their data pins **D2–D9** map D2→D0 … D9→D7.

**XCLK:** PA8 outputs 16 MHz. If the camera module has its own oscillator
(a small metal can marked e.g. 24.000), it already drives XCLK – leave the
XCLK wire off and set `OV5640_DRIVE_XCLK_FROM_MCO1` to 0 in
`Core/Inc/ov5640_conf.h`, otherwise two outputs fight on that wire.

## 4. TFT display + touch (TPM408-2.8, 14-pin header, left to right)

| # | Module pin | Connect to | Nucleo label |
|---|---|---|---|
| 1 | VCC | 5V | 5V |
| 2 | GND | GND | GND |
| 3 | CS | PD14 | D10 |
| 4 | RESET | PF12 | D8 |
| 5 | DC | PD15 | D9 |
| 6 | SDI (MOSI) | PB5 | **D22** (not D11) |
| 7 | SCK | PB3 | **D23** (not D13) |
| 8 | LED | 3V3 | 3V3 |
| 9 | SDO (MISO) | leave off (PB4 only for diagnostics) | D25 |
| 10 | T_CLK | PB3 | D23 – same as pin 7 |
| 11 | T_CS | PF13 | D7 |
| 12 | T_DIN | PB5 | D22 – same as pin 6 |
| 13 | T_DO | PB4 | D25 |
| 14 | T_IRQ | PE11 | D5 |

Pins 6/12 and 7/10 share one Nucleo pin each – join them on a breadboard
row or with a Y-jumper. Pin 9 is normally left off: the panel's SDO does
not release MISO and would disturb the touch controller.

## 5. SD card slot (4 pads at the bottom of the display module)

| Module pad | Connect to | Nucleo label |
|---|---|---|
| SD_SCK | PB3 | D23 (shared) |
| SD_MOSI | PB5 | D22 (shared) |
| SD_MISO | PB4 | D25 (shared) |
| SD_CS | **PF15** | **D2** |

Until this is wired, the SD icon shows grey with a red slash.

## 6. Buttons

**Capture** – 12 mm tactile button, 4 legs, joined in pairs inside. Use two
**diagonally opposite** legs:

| Leg | Connect to |
|---|---|
| one leg | **PF14** (D4) |
| diagonally opposite leg | **GND** |

No resistor needed (internal pull-up). Press = shoot / back to live,
starts or cancels the self-timer when TIMER is on. **Held while powering
up = re-run touch calibration.**

**Power** – the slide switch in the 5 V line, see section 1. These mini
switches are rated about 0.5 A, close to the board's draw; use a larger
toggle switch if it gets warm.

## 7. Battery gauge (optional)

For a 1-cell Li-ion/LiPo (3.0–4.2 V), powering the board through a 5 V
boost module (e.g. MT3608) into the slide switch:

```
Battery + ──[100 kΩ]──┬──[100 kΩ]── GND
                      ├── PA3 (A0)
                      └──[100 nF]── GND
```

With nothing on PA3 the status bar shows a plug icon instead of a battery.

## 8. Serial log (optional)

USART3 at **2 Mbaud**, 8N1:

| Adapter | Nucleo |
|---|---|
| RXD | PD8 (morpho CN12) |
| TXD | PD9 (morpho CN12) |
| GND | GND |
| VCC / 3.3V / 5V | **nothing** |

A plain CP2102 may not reach 2 Mbaud; a CP2102N does. The firmware works
without it – live statistics are also readable over SWD.

## 9. Flashing

With the ST-LINK wired and the board powered:

```
STM32_Programmer_CLI -c port=SWD mode=UR -w Debug/OV5640_F767ZI.elf -v -rst
```

`STM32_Programmer_CLI` ships with STM32CubeIDE under
`plugins/…cubeprogrammer…/tools/bin`. To erase the chip completely:
`STM32_Programmer_CLI -c port=SWD mode=UR -e all`.

On first boot (or after an erase) the touch calibration runs: tap the
three crosshairs firmly with a fingernail or stylus.

## 10. First power-up checklist

1. Supply measures 4.8–5.2 V, correct polarity, on the **5V** pin.
2. 3V3 to GND measures ~3.3 V with everything running.
3. Nothing gets hot. The STM32 should only feel warm.
4. Screen: red → green → blue → gradient → (calibration on first boot) →
   live camera image with the icon status bar.
5. Green LED LD1 toggles once a second while the preview runs.

## 11. LED codes

| LEDs | Meaning |
|---|---|
| Red blinks N times, repeating | Camera fault, N = `Camera_Status_t` (2 I2C, 3 chip ID, 4 config, 5 DMA, 6 timeout, …) – the screen also shows a struck-out camera |
| Red ×10, repeating | HSE requested but never ready |
| Both alternating | `Error_Handler` (unexpected HAL failure) |
| Both flashing fast | HardFault |
| Green toggling every second | Live preview running |
