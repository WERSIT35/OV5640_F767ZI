# Project status – checkpoint 2026-11-25

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
| Shutter | Freezes the picture for review; shows NOT SAVED while saving is off |
| SD card | Reads FAT and exFAT, CRC-checked; long names. **Saving is written but off** |
| Gallery | Lists JPG and BMP, shows baseline JPEG (hardware decoder) and 24/32-bit BMP, honours Exif rotation, uses a camera's embedded preview |

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
| SD card silent | SD_CS not connected, then MOSI and MISO swapped |

The cause of the earlier overheating was **not proven**. Candidates are the
power mistakes above and PA8 having been driven while the camera module has
its own clock. Keep PA8 and the camera's NC pin unconnected.

## Autofocus

No working focus motor was found: sweeping the sensor's VCM output over its
full range changed sharpness no more than holding it still, and no separate
driver chip answered on I2C. A motor with no supply cannot be ruled out.

## SD card, measured 2026-11-25

| Check | Result |
|---|---|
| Card | 119 GB exFAT from a Sony camera, 1190 files, 560 JPG listed |
| Reading | over 300,000 blocks at 13.5 MHz, 0 CRC errors, 0 retries |
| 24 MP JPEG | about 1 s through the preview, 16 s decoded in full |
| Card contents | unchanged: names, sizes, dates, first and last 4 KB of every file |
| Blocks written | 0 |

`PHOTO_SAVE_ENABLED` in `Core/Inc/photo.h` is **0**. The write path has never
run on hardware. Test it only on a spare or backed-up card: saving one photo
changes the file system that every other file depends on. Saving creates new
files only (`OV5640/IMG_nnnn.BMP`), reads each one back, and refuses to write
the partition table or boot sector; there is no delete or format code.

The gallery's touch buttons have not been confirmed by hand. RAW files,
videos and progressive JPEGs are not shown.

## Next steps

1. 15-minute run, then re-check voltage, temperature and errors.
2. Battery: TP4056 + MT3608 at 5.0 V (WIRING.md section 1 and 7).
3. Photo saving: test on a spare or backed-up card, then set
   `PHOTO_SAVE_ENABLED` to 1. Saving inside the STM32's flash is an
   alternative that never writes to the card.
4. Ideas: higher-resolution JPEG photos, auto sleep, backlight
   dimming, video.
