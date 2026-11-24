# Wiring guide

Everything needed to wire the OV5640 camera, the 2.8" ILI9341 touch display,
the buttons, power and the debugger to a **NUCLEO-F767ZI**.

Arduino-style labels (D2, D23, A0 …) are printed on the Nucleo next to the
Zio headers. Pins with no such label are on the long two-row **morpho**
headers along the edges (CN11 on the left, CN12 on the right); their names
are printed on the underside of the board.

> **Read the power section first.** Swapped, missing or wrong-voltage supply
> wires caused most of the faults on this build. Check polarity and voltage
> with a meter before the first power-up of any new wiring.

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
- Keep jumper **JP5 in**. Without it the chip can be fed through its signal
  pins, which heats it.
- The 5V pin must really get 5 V: with 3 V on it the chip ran at 3.02 V.
- Power up: 5 V first, then the ST-LINK USB. Power down: ST-LINK USB first.

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
| NC | **nothing** | PA8 also stays unconnected, see below |
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

A pin printed **PWNN** is PWDN. If SDA and SCL are swapped the camera does
not answer at all.

**XCLK:** this camera module has its own oscillator and no XCLK pin, so PA8
is left off and `OV5640_DRIVE_XCLK_FROM_MCO1` is 0 in
`Core/Inc/ov5640_conf.h`. For a module with an XCLK pin and no oscillator,
wire XCLK to PA8 and set it to 1 (16 MHz). Never do that on a module with
its own oscillator: two outputs would fight on that wire.

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

The SD icon appears only when a card is found. Photos are not saved yet.

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

**Power** – the slide switch. On an external 5 V supply it sits in the 5 V
line (section 1); on battery, before the boost module (section 7). Mini
slide switches are often rated about 0.5 A; use 1.5 A or more on battery.

## 7. Battery (planned, not yet built)

Remove the external 5 V supply's 5V and GND wires first; never connect both.

| From | To |
|---|---|
| Battery + / − | TP4056 (with protection) B+ / B− |
| TP4056 OUT+ | switch, middle pin |
| Switch, outer pin | MT3608 IN+ |
| TP4056 OUT− | MT3608 IN− |
| MT3608 OUT+ | Nucleo 5V (never VIN or 3V3) |
| MT3608 OUT− | Nucleo GND |

One 3.7 V Li-ion/LiPo cell able to supply 1.5 A. Set the MT3608 to **5.0 V
before connecting the Nucleo**. Put 470 µF across its output. Switch off
while charging.

Gauge, taken after the switch:

```
MT3608 IN+ ──[100 kΩ]──┬──[100 kΩ]── GND
                       ├── PA3 (A0)
                       └──[100 nF]── GND
```

With nothing on PA3 the status bar shows a plug icon instead of a battery.

## 8. Serial port

Not used any more: the UART log and frame dump were removed from the
firmware. Statistics (`preview_stats`, `power_raw`) are readable over SWD.

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
4. Screen: lens animation with a loading bar, touch calibration on first
   boot, then the live picture.
5. Green LED LD1 toggles once a second while the preview runs.

## 11. LED codes

| LEDs | Meaning |
|---|---|
| Red blinks N times, repeating | Camera fault, N = `Camera_Status_t` (2 I2C, 3 chip ID, 4 config, 5 DMA, 6 timeout, …) – the screen also shows a struck-out camera |
| Red ×10, repeating | HSE requested but never ready |
| Both alternating | `Error_Handler` (unexpected HAL failure) |
| Both flashing fast | HardFault |
| Green toggling every second | Live preview running |
