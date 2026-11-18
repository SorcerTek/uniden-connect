# Uniden Connect

A touchscreen remote for the **Uniden SDS200** scanner, running on the
**Freenove ESP32-S3 3.5" display (FNK0104N)**.

What it does:

- Shows what the scanner is receiving: system, department, channel / talkgroup, frequency, signal.
- Touch buttons for the scanner's keys (on the KEYS page): System / Dept / Channel hold, Avoid,
  Func, Replay, Menu, the scroll knob (< PUSH >), number keys, Zip, Range.
  Hold a scanner button for about 1 second to send a "long press".
- Board speaker volume and mute right on the main screen.
- Boot animation while it connects, and a power menu on the BOOT button (no power switch needed).
- Plays the scanner's audio on the board's own speaker, over Wi-Fi.

The scanner is expected at **192.168.1.2**. Change it in `include/config.h` if that ever changes.

---

## 1. One-time setup on your Mac

1. **Install VS Code** from <https://code.visualstudio.com> and open it.
2. **Install PlatformIO**: click the Extensions icon on the left (four squares), search for
   **PlatformIO IDE**, click **Install**. Wait until it says it's finished (it downloads a lot the
   first time, 5 to 10 minutes). Restart VS Code when it asks.
3. You'll now see an **alien-head icon** in the left bar. That's PlatformIO.

## 2. Open the project

1. In VS Code: **File > Open Folder...** and pick the `Uniden Connect` folder
   (the one that contains `platformio.ini`).
2. PlatformIO notices the project and downloads the ESP32 tools and the LovyanGFX library on its own.
   Let it finish (watch the bottom status bar).

## 3. Put in your Wi-Fi details

Open `include/secrets.h` and change the two lines:

```c
#define WIFI_SSID     "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"
```

The board must join the same network as the scanner. The ESP32 only works with **2.4 GHz** Wi-Fi.

## 4. Build and flash

1. Plug the board into the Mac with a USB-C **data** cable.
2. Click the **checkmark** (✓) in the blue bar at the bottom of VS Code. That's **Build**.
   The first build takes a few minutes. It should end with `SUCCESS`.
3. Click the **right arrow** (→) next to it. That's **Upload**.
   - If the upload can't connect: hold the board's **BOOT** button, tap **RESET**, let go of BOOT,
     then click Upload again. Press RESET once more when it's done.
4. Click the **plug icon** (🔌) to open the **Serial Monitor**. You'll see start-up messages,
   the board's IP address, and the scanner's audio stream details.

## 5. Scanner settings

On the SDS200, make sure the network is set up and it has the IP **192.168.1.2**
(Menu > Settings > Network / LAN). The firmware uses:

| What | How | Port |
|---|---|---|
| Status and key presses | UDP "virtual serial" commands (`GSI`, `KEY`, `JPM`, `MSM`) | 50536 |
| Audio | RTSP stream `rtsp://192.168.1.2/au:scanner.au` (RTP over UDP) | 554 |

Only one app can usually pull the audio stream at a time, so close ProScan / VLC / Sentinel audio
if the board shows "PLAY failed" or "SETUP failed".

## 6. Using it

**Starting up**: the Uniden Connect logo appears, then the boot animation plays while the board
joins Wi-Fi and finds the scanner (the line at the bottom says which step it's on). As soon as the
scanner answers it switches to the main screen. If the scanner hasn't answered after 25 seconds it
goes to the main screen anyway, which shows "Scanner not responding" until it does.

**Main screen**

- Top bar: green dot = scanner answering, second dot = audio stream (green while audio plays),
  SPK = board speaker volume, battery, bars = signal.
- Middle: system (blue), department (yellow), channel (big), then frequency / TGID / mode,
  service type, and site / RSSI / scanner info. An orange **HOLD** tag shows what's held.
- Buttons: SPK - / SPK + (board speaker volume), MUTE (board speaker only), KEYS.
- **WX** (above KEYS) puts the scanner in NOAA weather mode: it checks the weather channels and stops
  on the one it hears, and the screen shows "NOAA Weather" with that frequency. **SCAN** (above MUTE)
  puts it back to normal scanning. The one that's active is outlined in orange.
  The scanner's own volume and squelch are left as set on the scanner.

**KEYS screen**: number pad, ".", E/YES on the left; SYSTEM, DEPT, CHAN (hold / release),
the scroll knob (< PUSH >), MENU, FUNC, AVOID, ZIP, RANGE, REPLAY on the right; along the bottom
RECONNECT AUDIO, REBOOT SCANNER (tap twice) and BACK.

**Power (BOOT button)**: hold the board's **BOOT** button for about 1.5 seconds to open the power menu:

- **POWER OFF**: closes the audio stream cleanly, turns off the screen and speaker, and puts the
  board into deep sleep. Press **BOOT** once to turn it back on (it starts up like a fresh power-on).
- **RESTART**: closes the audio stream cleanly and restarts the board.
- **CANCEL** (or a short press of BOOT) goes back.

"Off" is a very low-power sleep, not a true power cut: the battery charger and screen circuitry still
draw a little. If you'll store it for weeks, unplug the battery.

**Battery**: the battery icon at the top right shows the battery level; "--" means there is no battery
reading. If the % looks off, adjust `BATTERY_DIVIDER` in `include/config.h`.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Screen stays black | Make sure `platformio.ini` wasn't changed. Unplug the USB cable for 5 seconds (this panel only resets on power-off). |
| Picture upside down | In `include/config.h` change `SCREEN_ROTATION` from 1 to 3. |
| Colours look wrong | Set `LCD_SWAP_BYTES` to 1 in `include/config.h`. |
| Taps land in the wrong spot | Set `TOUCH_DEBUG` to 1, watch the Serial Monitor while you tap, then try `TOUCH_INVERT_X` / `TOUCH_INVERT_Y`. |
| "Scanner not responding" | Check the scanner IP, that the board's on the same network, and that the scanner's network is on. |
| No sound but "streaming" | Raise SPK on the main screen, check the speaker plug, and check the scanner's volume isn't 0. |
| "Connection reset by peer" / audio won't come back after the board was reset or unplugged | The scanner only allows one audio listener and is still holding the old one. Use **RESTART** / **POWER OFF** from the BOOT menu (they close it cleanly); otherwise tap REBOOT SCANNER on the KEYS page. |
| Want a different boot animation | Replace `Uniden Connect Bootup.gif` / `Uniden Connect boot logo.jpeg`, run `sh tools/make_boot_assets.sh` (needs ffmpeg), then build and upload. |
| Choppy sound | Raise `AUDIO_PREBUFFER_MS` in `include/config.h` (e.g. 500). Move the board closer to the router. |
| Serial Monitor is blank | Press RESET on the board after opening the monitor. Make sure `-DARDUINO_USB_CDC_ON_BOOT=1` is in `platformio.ini`. |

---

## Project layout

```
platformio.ini          build settings (board, memory, libraries)
include/config.h        scanner IP, ports, screen/touch/audio options, board pins
include/secrets.h       your Wi-Fi name and password (kept out of GitHub)
src/main.cpp            start-up: screen, touch, audio, Wi-Fi, scanner
src/sds200.*            talks to the scanner over UDP 50536, reads GSI status
src/rtsp_audio.*        connects to the scanner's RTSP audio stream
src/audio_out.*         ES8311 codec + speaker amplifier + jitter buffer
src/display_st77922.*   driver for the ST77922 QSPI screen
src/touch_st77922.*     driver for the screen's touch controller
src/ui.*                screens, buttons, touch handling, boot animation
src/power.*             BOOT button power menu (power off = deep sleep, restart)
src/battery.*           battery level from the ADC
data/boot_logo.jpg      boot logo, built into the firmware
data/boot_anim.bin      boot animation frames, built into the firmware
tools/make_boot_assets.sh  rebuilds the two files above from the GIF / logo
```

## Sources

- Uniden, *SDS200 Virtual Serial on Network Protocol Specification* V1.00 (UDP 50536 framing).
- Uniden, *SDS100/SDS200/SDS150 Remote Command Specification* V2.00 (GSI, KEY, VOL, SQL).
- Freenove FNK0104 docs (<https://docs.freenove.com/projects/fnk0104/>) and the community
  FNK0104N notes at github.com/emailman/Freenove_FNK0104N_MicroPython (ST77922 init table,
  touch registers, audio pins).
- Espressif esp-bsp ES8311 driver (codec clock tables, Apache-2.0).
