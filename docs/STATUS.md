# Project status – checkpoint 2026-11-24

The camera was rewired from scratch and works. The STM32 stays cool.

## Measured on hardware

| Check | Result |
|---|---|
| Camera / display | 27–28 fps, 0 overruns, 0 short and 0 dropped frames in normal running |
| Chip supply (VDDA) | 3.30–3.31 V |
| Die temperature | 36–38 °C (about 87 °C on the old wiring) |
| Camera ID | 0x5640 at I2C address 0x3C |
| Touch | calibrated and working |
| Night mode | about 9 fps, by design |

The longest continuous run checked over SWD was a few minutes; a 15-minute
run is still to be done.

## Firmware

| Area | State |
|---|---|
| Start-up | Lens animation and loading bar; hardware self-tests removed |
| Picture | Standard / Vivid / Natural styles, 50 Hz anti-flicker, lower gain ceiling, lens colour shading tuned on this lens |
| Zoom | 1x / 2x / 4x by sensor crop |
| Interface | Status bar, round shutter, three-tab menu, tap the picture to hide controls |
| Settings | Saved to flash sector 10 three seconds after a change |
| Robustness | Camera start-up retries, and a restart if frames stop for 3 s |
| Shutter | Freezes the picture for review. **Photos are not saved yet** |
| SD card | Presence icon only |

Checked from frames read out of RAM: main screen, System tab, 1x/2x/4x,
Vivid and night. Not yet confirmed by hand: the intro, every touch button,
settings surviving a power cycle, and whether the picture is the right way
up (Menu > Tools > Rotate).

## Faults found while rewiring

| Symptom | Cause |
|---|---|
| VDDA 3.02 V | 3 V had been connected to the 5V input |
| SDA and SCL stuck low | Camera supply connection; fixed after rework and cleaning |
| Camera did not answer (NACK) | SDA and SCL swapped |
| White screen | Display SCK unplugged |

The cause of the earlier overheating was **not proven**. Candidates are the
power mistakes above and PA8 having been driven while the camera module has
its own clock. Keep PA8 and the camera's NC pin unconnected.

## Autofocus

No working focus motor was found: sweeping the sensor's VCM output over its
full range changed sharpness no more than holding it still, and no separate
driver chip answered on I2C. A motor with no supply cannot be ruled out.

## Next steps

1. 15-minute run, then re-check voltage, temperature and errors.
2. Battery: TP4056 + MT3608 at 5.0 V (WIRING.md section 1 and 7).
3. SD card wiring (section 5), then saving photos.
4. Ideas: gallery, higher-resolution JPEG photos, auto sleep, backlight
   dimming, video.
