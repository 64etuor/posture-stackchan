# posture-stackchan

Desktop posture alert device built on an **M5Stack CoreS3 (K128)**.

A program on the PC watches the webcam, scores forward-head posture, and pushes
alerts to the CoreS3 over Wi-Fi. The CoreS3 shows a StackChan-style face whose
expression follows the posture state and beeps when the alert fires.
**The CoreS3 never judges posture itself** — it only renders state it is told about.

## Current status: stages 1–5 complete

The device is feature-complete for v1 and the PC monitor drives it.

| What | Behaviour |
| --- | --- |
| Boot | `STARTING` bar, sleepy face, ~1.2 s |
| Connecting | `CONNECTING` cyan bar; a one-off 2.4 GHz scan is logged for diagnosis |
| Idle | `POSTURE OK` green bar, round eyes, smile, random blinking, IP under the face |
| Slouching | `CHECK POSTURE` yellow bar, worried brows — silent |
| Alert | `STRAIGHTEN UP` red bar, angry brows, open mouth, two short beeps |
| Notice | stretch / meal message in Korean with a rising chime |
| Muted | sleepy eyes with a live `MUTED 9:58` countdown |
| Wi-Fi lost | retry with 1→30 s backoff, recovers on its own |
| No PC for 30 s | `PC OFFLINE` with X eyes; the sub-line separates Wi-Fi from PC |

Touch (spec section 7):

| Gesture | Effect |
| --- | --- |
| tap during an alert | silence it and mute for the configured time |
| tap while muted | unmute |
| long press (0.8 s) on the face | fire a local test alert — works offline too |
| tap the top-right corner | open the settings screen |
| tap a notice | dismiss it early |

The settings screen adjusts volume, brightness, mute duration and the alert
sound, all persisted to NVS and restored on boot.

Everything is driven from `millis()`; there is no blocking `delay()` longer than
5 ms, so the blink animation, the beep pattern, Wi-Fi retries, the HTTP server
and the mute countdown never stall each other.

## Relationship to the official StackChan project

[`m5stack/StackChan`](https://github.com/m5stack/StackChan) is an **ESP-IDF v5.5.4**
project (`idf.py` build, no `platformio.ini`) covering a full AI robot: dual
servos, NFC, camera, mobile app and a backend server. This project targets a much
narrower job on a PlatformIO + Arduino + M5Unified stack, so it reproduces the
StackChan *face UI style* with M5GFX drawing primitives rather than forking that
firmware. No third-party avatar library is used, which keeps the render loop
inside our own non-blocking main loop.

## Requirements

- PlatformIO Core 6.x (`pip install platformio`) or the VS Code PlatformIO extension
- USB-C **data** cable (charge-only cables will not enumerate a COM port)

Pinned toolchain (see `platformio.ini`):

- `platform = espressif32@6.9.0`
- `board = m5stack-cores3`
- `m5stack/M5Unified@0.2.7`
- `bblanchon/ArduinoJson@7.4.3`

Two board settings are load-bearing and explained in `platformio.ini`: PSRAM is
**quad** SPI on this board (`qio_qspi`), and flashing uses `--no-stub` because
esptool's RAM stub is unreliable over the CoreS3 USB-Serial/JTAG link.

## Build, upload, monitor

```bash
pio run                       # compile
pio run --target upload       # flash over USB-C
pio device monitor            # serial log at 115200
pio run --target upload --target monitor   # flash then monitor
```

If the board is not detected:

1. Confirm the USB-C cable carries data.
2. Check the COM port in Windows Device Manager.
3. Pin the port explicitly: `pio run -t upload --upload-port COM5`.
4. Enter download mode: hold the reset button ~2 s until the green LED lights, then release.

## Expected serial output

```
[BOOT] CoreS3 initialized
[BOOT] heap=324580 bytes psram=8386231 bytes
[DISPLAY] UI ready (320x240)
[AUDIO] speaker ready (enabled=1 volume=120)
[STATE] init -> BOOTING
[BOOT] setup complete
[WIFI] Scanning 2.4 GHz band (try 1)
[STATE] BOOTING -> CONNECTING (boot complete)
[WIFI] Scan found 6 network(s) reachable on 2.4 GHz:
[WIFI]   "KT_GiGA_38BB" ch=5 rssi=-48 auth=4
[WIFI] Connecting to KT_GiGA_38BB (attempt 1)
[WIFI] Connected: 172.30.1.61 (rssi -48 dBm)
[STATE] CONNECTING -> NORMAL (wifi connected)
[TOUCH] pressed at x=160 y=120
[STATE] NORMAL -> ALERT (touch test)
[AUDIO] alert pattern started
[AUDIO] alert sound played
[STATE] ALERT -> NORMAL (alert timeout)
```

## Layout

```
include/  app_state.h  display_ui.h  audio.h  network.h  command_handler.h
          settings.h  logging.h  secrets.example.h
src/      main.cpp  app_state.cpp  display_ui.cpp  audio.cpp
          network.cpp  command_handler.cpp  settings.cpp
pc/       monitor.py  posture.py  device.py  voice.py  reminders.py  config.py
docs/     protocol.md  troubleshooting.md
tools/    capture_serial.py  test_http.ps1  test_notify.ps1
```

`main.cpp` only wires modules together and owns the transition rules. Face
rendering, sound, Wi-Fi, the HTTP protocol and persistence each live in their
own translation unit.

`tools/` holds the test harnesses used to verify the firmware:
`test_http.ps1` exercises all 14 protocol cases against a live device while
capturing the serial log, and `test_notify.ps1` does the same for notices.

## PC monitor

See [`pc/README.md`](pc/README.md). In short:

```powershell
cd pc
pip install -r requirements.txt
copy config.example.json config.json   # then set device_url
python monitor.py --preview
```

It calibrates against *your* neutral posture, pushes `NORMAL` / `BAD_PENDING` /
alerts to the device, tracks sitting time for stretch reminders, and speaks
meal reminders through the PC speakers with the Korean Windows voice.

## State machine

```
BOOTING -> CONNECTING -> NORMAL
NORMAL  -> BAD_PENDING -> ALERT -> NORMAL
ALERT   -> MUTED -> NORMAL
any     -> NOTICE -> back to the posture face
any     -> OFFLINE | ERROR
```

`NOTICE` is not in the original spec list; it carries the stretch and meal
messages. Like `ALERT` it is a transient overlay that always falls back to
whatever the PC last reported.

Transitions go through `StateMachine::transitionTo()` exclusively, which is also
the only place that logs `[STATE] A -> B`.

> The `ERROR` state is spelled `AppState::FAULT` in code because `ERROR` is a
> macro in several ESP32 headers. Logs and the on-screen label still read `ERROR`.

## Roadmap

| Stage | Content |
| --- | --- |
| 1 | face, blink, touch, beep — **done** |
| 2 | Wi-Fi connect, IP on screen, auto-reconnect with backoff — **done** |
| 3 | HTTP server, stale + duplicate `alert_id` rejection — **done**, all 14 contract cases verified |
| 4 | touch-to-mute with countdown, settings screen, NVS persistence — **done** |
| 5 | PC MediaPipe monitor, sitting timer, stretch and meal voice reminders — **done** |
| 6 | 8 h soak test, reconnect and memory-leak checks — **outstanding** |

Not yet verified on hardware: Wi-Fi auto-reconnect after a router power cycle,
and the 8 h soak. Both belong to stage 6.

The HTTP contract is written up in [`docs/protocol.md`](docs/protocol.md), and
the hardware gotchas that cost real time are in
[`docs/troubleshooting.md`](docs/troubleshooting.md).
