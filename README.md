# Uniden Connect

A touchscreen remote for the **Uniden SDS200** scanner, running on the
**Freenove ESP32-S3 3.5" display (FNK0104N)** or the **M5Stack Tab5** (5", 1280x720).

What it does:

- Shows what the scanner is receiving: system, department, channel / talkgroup, frequency, signal.
- Touch buttons for the scanner's keys (on the KEYS page): System / Dept / Channel hold, Avoid,
  Func, Replay, Menu, the scroll knob (< PUSH >), number keys, Zip, Range.
  Hold a scanner button for about 1 second to send a "long press".
- Board speaker volume and mute right on the main screen, plus WX (NOAA weather) and SCAN.
- Plays the scanner's audio on the board's own speaker, over Wi-Fi.
- Logs every call, records the audio to the microSD card, and lists recent calls you can replay.
- Weather alert banner when the NOAA 1050 Hz warning tone is heard.
- Favorites: one tap to hold the scanner on a saved department.
- Clock, screen dimming when idle, settings remembered across restarts.
- On-screen Wi-Fi / scanner setup, wireless (Wi-Fi) firmware updates.
- Boot animation while it connects, and a power menu (BOOT button, or KEYS > POWER).
- Wi-Fi signal icon in the top bar; the setup screen shows the board's own IP address.

The scanner is expected at **192.168.1.2** by default. Change it on the board's setup screen.

**Current version: v2.0.1** (shown under SETTINGS and on the boot screen).

| Version | Date | What changed |
|---|---|---|
| v2.0.1 | 2026-11-19 | Tab5: after auto-rotating, the whole screen is redrawn (no upside-down "ghost" top bar over the buttons). |
| v2.0.0 | 2026-11-19 | M5Stack Tab5 support (all display versions, Launcher install, smooth 1280x720 display, auto-flip, louder speaker). Both boards: POWER button on the KEYS page, Wi-Fi icon, board IP on the setup screen, instant volume beep. |
| v1.0.0 | 2026-11-18 | First release for the Freenove FNK0104N. |

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

## 3. Wi-Fi details (on the board)

You don't need to put anything in the code. The first time the board starts without saved Wi-Fi,
it opens the **setup screen**:

1. Tap **Wi-Fi network**, pick your network from the list (or **OTHER...** to type a hidden one).
2. Type the password on the on-screen keyboard (**SHOW** shows what you typed), tap **OK**.
3. Check **Scanner IP** (default 192.168.1.2).
4. Optional: **Update password** turns on wireless updates (see section 7).
5. Tap **SAVE & CONNECT**. The board tries the network first and only saves it if it connects.

You can come back any time: **KEYS > SETTINGS > WI-FI SETUP**. **FORGET WI-FI** clears it.
The board must join the same network as the scanner. The ESP32 only works with **2.4 GHz** Wi-Fi.

`include/secrets.h` is optional. If it's filled in, the board copies those details into its own
memory the first time it starts. It's never uploaded to GitHub, but a compiled firmware file
would contain it, so empty it before sharing a build with anyone.

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

On the SDS200, make sure the network is set up and check the scanners IP address. 
(It is HIGHLY recommended to set a reserved IP address for your scanner in your router).
(Menu > Settings > Network / LAN). The firmware uses:

| What | How | Port |
|---|---|---|
| Status and key presses | UDP "virtual serial" commands (`GSI`, `KEY`, `JPM`, `HLD`, `MSM`) | 50536 |
| Audio | RTSP stream `rtsp://192.168.1.2/au:scanner.au` (RTP over UDP) | 554 |

Only one app can usually pull the audio stream at a time, so close ProScan / VLC / Sentinel audio / or the Siren app,
if the board shows "PLAY failed" or "SETUP failed". If the audio still isn't working tap keys / settings / reboot scanner (requires a double tap)

## 6. Using it

**Starting up**: the Uniden Connect logo appears, then the boot animation plays while the board
joins Wi-Fi and finds the scanner (the line at the bottom says which step it's on). As soon as the
scanner answers it switches to the main screen. If the scanner hasn't answered after 25 seconds it
goes to the main screen anyway, which shows "Scanner not responding" until it does.

**Main screen**

- Top bar: green dot = scanner answering, the time (shown once the scanner is connected; "SDS200" until then), second dot = audio stream (green while audio
  stream is active), a red **REC** while a call is being recorded, SPK = board speaker volume, battery, signal bars.
- Middle: system (blue), department (yellow), channel (big), then frequency / TGID / mode,
  service type, and site / RSSI / scanner info. An orange **HOLD** tag shows what's held.
- Buttons: SPK - / SPK + (board speaker volume), MUTE (board speaker only), KEYS.
  The scanner's own volume and squelch are left as set on the scanner.
- **WX** (above KEYS) puts the scanner in NOAA weather mode: it checks the weather channels and stops
  on the one it hears. **SCAN** (above MUTE) goes back to normal scanning and releases any
  System / Department / Channel hold. The one that's active is outlined in orange.
- If the scanner keeps refusing the audio connection (see Troubleshooting), a red
  **AUDIO STUCK: REBOOT SCANNER** button appears; tap it twice.

**KEYS screen**: number pad, ".", E/YES on the left; SYSTEM, DEPT, CHAN (hold / release),
the scroll knob (< PUSH >), MENU, FUNC, AVOID, ZIP, RANGE, REPLAY on the right. Along the bottom:

- **REC ON / REC OFF**: record calls to the SD card or not (remembered).
- **RECENT**: the last 20 calls.
- **FAVS**: favorites.
- **SETTINGS**: brightness, dimming, Wi-Fi setup, and the recovery buttons.
- **BACK**.

**Recent calls**: newest first, with the time, channel, department and length. Tap a call with a
green arrow to play its recording through the speaker (live audio pauses while it plays); tap it
again or **STOP** to stop. UP / DOWN scroll. After a restart the list is filled from today's log.

**Favorites**: six slots. While the scanner is on a department you like, **hold a slot for 1 second**
to save it (if there's no department, e.g. on a trunked system between calls, the whole system is
saved). **Tap** a saved slot and the scanner jumps there and **holds** on that department (or system).
Tap **SCAN** to go back to normal scanning.

**Key light**: the buttons look like backlit scanner keys. Under **SETTINGS**, tap one of the ten
sample keys (Amber, Orange, Red, Yellow, Green, Ice, Blue, Purple, Pink, White) to change the
backlight color; it changes straight away and is remembered. Active buttons (MUTE on, SCAN/WX,
held SYSTEM/DEPT/CHAN, REC ON) show a lit bar along the top.

**Settings**: Brightness, Dim after (Never / 30 s / 1 / 2 / 5 / 10 min) and Dimmed level (Off or
5-50%). The screen dims when nothing has happened for that long (no signal, no channel change, no
touch) and wakes on activity or a touch. The first touch on a dim screen only wakes it.
Below that: Wi-Fi and IP, scanner IP, SD card space, wireless update status, and the buttons
**WI-FI SETUP**, **RECONNECT AUDIO**, **REBOOT SCANNER** (tap twice).

All settings (speaker volume, mute, brightness, dimming, recording, favorites, Wi-Fi, scanner IP)
are saved on the board and survive restarts and power-off.

**Clock**: set automatically over the internet once Wi-Fi is up (US Central time; change
`TIMEZONE` in `include/config.h` if you move).

**Weather alert**: when the board hears the NOAA Weather Radio warning tone (1050 Hz for 1 second or
more), a flashing red **WEATHER ALERT** banner covers the screen with the channel and frequency, the
screen wakes, the board speaker un-mutes, and it beeps every few seconds until you tap the banner.
The board can only hear the tone when the scanner is actually on the weather channel: either you've
pressed **WX**, or you've turned on the scanner's own weather-alert priority, which makes the SDS200
check the weather channel while it scans and switch to it when an alert starts.

**microSD card** (FAT32). The board creates:

```
/logs/2026-11-18.csv                    one line per call: date, time, seconds, system, department,
                                        channel, TGID, frequency, recording file
/rec/2026-11-18/033540_NOAA_Weather.wav  the call's audio (8 kHz mono WAV) - opens in any audio player
```

A call is only recorded while real audio comes through: it starts when the audio is louder than
`REC_SILENCE_RMS` (in `include/config.h`), ends after 2 seconds of silence or when the channel
changes, and calls shorter than 1 second are thrown away. Nothing is recorded while the scanner is
just scanning. A WAV is about 1 MB per minute, so 32 GB holds hundreds of hours. Delete old
folders on a computer when you like.

**Power (BOOT button)**: hold the board's **BOOT** button for about 1.5 seconds to open the power menu:

- **POWER OFF**: finishes any recording, closes the audio stream cleanly, turns off the screen and
  speaker, and puts the board into deep sleep. Press **BOOT** once to turn it back on.
- **RESTART**: the same clean shutdown, then a restart.
- **CANCEL** (or a short press of BOOT) goes back.

"Off" is a very low-power sleep, not a true power cut: the battery charger and screen circuitry still
draw a little. If you'll store it for weeks, unplug the battery.

**Battery**: the battery icon at the top right shows the battery level; "--" means there is no battery
reading. If the % looks off, adjust `BATTERY_DIVIDER` in `include/config.h`.

## 7. Updating over Wi-Fi (no USB cable)

Once the board is on Wi-Fi and has an **update password** (KEYS > SETTINGS > WI-FI SETUP), you can
upload new firmware without plugging it in:

1. Put the same password in `secrets.ini` in the project folder (copy `secrets.example.ini` if it
   isn't there):
   ```ini
   [ota]
   password = your-update-password
   ```
2. In VS Code's blue bottom bar, click the environment name (it says `env:fnk0104n`) and choose
   **env:fnk0104n_ota**.
3. Click **Upload** (→). The board shows "Updating... NN%", then restarts with the new version.
4. Switch back to **env:fnk0104n** for USB uploads.

The board must be switched on and on the same network as your computer. It's found as
`uniden-connect.local`. `secrets.ini` is never uploaded to GitHub.

---

## 8. M5Stack Tab5 version

The same firmware also runs on the M5Stack Tab5 (all display versions: ILI9881C, ST7123 and
ST7121 are detected automatically). Everything works the same as on the 3.5" board, with these
differences:

- The screens are scaled up to the 5" 1280x720 display with larger, smooth fonts. The picture
  turns over automatically when you turn the Tab5 upside down (`TAB5_AUTO_ROTATE`).
- There is no BOOT button to hold: open the power menu from **KEYS > POWER**. It also has
  **LAUNCHER** (back to bmorcelli's Launcher) when installed under it. Turn on with the Tab5's power button.
- Battery level comes from the Tab5's battery pack (NP-F550), if fitted.
- The speaker is louder than M5Stack's default (`TAB5_SPK_GAIN`); the 3.5 mm jack carries the same audio.
- Wireless (OTA) updates are off on the Tab5: install new versions through the Launcher.
- Bluetooth speakers aren't possible (the Tab5's ESP32-C6 is Bluetooth Low Energy only, no audio
  streaming); use a Bluetooth transmitter on the 3.5 mm jack instead.

**Building the Tab5 version**

1. Once, build the Tab5 libraries package (downloads Espressif's official Arduino-ESP32 3.3.10
   libraries for the Tab5's chip, with 200 MHz PSRAM and the matching Wi-Fi driver):
   ```bash
   python3 tools/make_tab5_libs.py
   ```
2. In VS Code pick **env:tab5** and click **Build**. The result is `.pio/build/tab5/firmware.bin`.
3. Copy that file to the Tab5's SD card and install it from bmorcelli's **Launcher**
   (SD > select the file > Install).

Note: PlatformIO's built-in Python can't unpack the P4 compiler download (it lacks `lzma`
support). If a Tab5 build stops with `riscv32-esp-elf-g++: command not found`, unpack
`~/.platformio/dist/riscv32-esp-elf-*.tar.xz` with the Mac's `tar` into
`~/.platformio/packages/toolchain-riscv32-esp`, or run PlatformIO from a Python 3.10+ that has
`lzma`. On macOS, VS Code also needs **Local Network** permission (System Settings > Privacy &
Security) for wireless uploads.

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
| "scanner busy" / audio won't come back after the board was reset or unplugged | The scanner only allows one audio listener and keeps holding the old one until it restarts. Use **RESTART** / **POWER OFF** from the BOOT menu and wireless updates (they close it cleanly). After a USB upload, a pulled battery or the RESET button, tap the red **AUDIO STUCK** button (or REBOOT SCANNER under SETTINGS). |
| Hiss or static gets recorded | Raise `REC_SILENCE_RMS` in `include/config.h` (e.g. 500). Quiet calls missed: lower it (e.g. 200). |
| "no SD card" | The card must be FAT32 (32 GB and smaller usually are). Re-seat it and restart the board. |
| Wrong time | Change `TIMEZONE` in `include/config.h`. The clock needs internet access through your Wi-Fi. |
| Want a different boot animation | Replace `Uniden Connect Bootup.gif` / `Uniden Connect boot logo.jpeg`, run `sh tools/make_boot_assets.sh` (needs ffmpeg), then build and upload. |
| Choppy sound | Raise `AUDIO_PREBUFFER_MS` in `include/config.h` (e.g. 500). Move the board closer to the router. |
| Serial Monitor is blank | Press RESET on the board after opening the monitor. Make sure `-DARDUINO_USB_CDC_ON_BOOT=1` is in `platformio.ini`. |

---

## Project layout

```
platformio.ini          build settings (board, memory, libraries)
include/config.h        scanner IP, ports, screen/touch/audio options, board pins
include/secrets.h       optional first-boot Wi-Fi / update password (kept out of GitHub)
secrets.ini             update password for wireless uploads (kept out of GitHub)
src/main.cpp            start-up: screen, touch, audio, Wi-Fi, scanner
include/board.h         what differs between boards (screen, canvas, touch, brightness)
src/board_fnk0104n.cpp  FNK0104N board layer
src/board_tab5.cpp      Tab5 board layer: M5GFX display (fast frame-buffer updates), touch,
                        M5.Speaker audio, battery, power, auto-rotate
partitions_tab5.csv     Tab5 partition table for USB flashing (the Launcher uses its own)
tools/make_tab5_libs.py builds the Tab5 libraries package (200 MHz PSRAM)
src/sds200.*            talks to the scanner over UDP 50536, reads GSI status
src/rtsp_audio.*        connects to the scanner's RTSP audio stream
src/audio_out.*         ES8311 codec + speaker amplifier + jitter buffer
src/display_st77922.*   driver for the ST77922 QSPI screen
src/touch_st77922.*     driver for the screen's touch controller
src/ui.*                screens, buttons, touch handling, on-screen keyboard, boot animation
src/settings.*          settings saved in the board's flash (NVS)
src/calls.*             call detection, SD card log + recordings, replay, weather-alert tone
src/ota.*               wireless updates
src/power.*             BOOT button power menu (power off = deep sleep, restart)
src/battery.*           battery level from the ADC
data/boot_logo.jpg      boot logo, built into the firmware
data/boot_anim.bin      boot animation frames, built into the firmware
data/tab5_boot_*        640x360 versions for the Tab5 (shown doubled, hardware JPEG decoding)
tools/make_boot_assets.sh  rebuilds the boot files above from the GIF / logo
```

## Sources

- Uniden, *SDS200 Virtual Serial on Network Protocol Specification* V1.00 (UDP 50536 framing).
- Uniden, *SDS100/SDS200/SDS150 Remote Command Specification* V2.00 (GSI, KEY, VOL, SQL).
- Freenove FNK0104 docs (<https://docs.freenove.com/projects/fnk0104/>) and the community
  FNK0104N notes at github.com/emailman/Freenove_FNK0104N_MicroPython (ST77922 init table,
  touch registers, audio pins).
- Espressif esp-bsp ES8311 driver (codec clock tables, Apache-2.0).
