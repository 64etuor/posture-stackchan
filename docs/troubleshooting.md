# Troubleshooting

Findings that cost real time on this hardware, with the evidence that settled them.

## The screen is black / `psram=0 bytes` in the boot log

CoreS3 has 8 MB of PSRAM and it is **quad** SPI, not octal.

`m5stack-cores3` ships with an empty `board_build.arduino.memory_type`, and it is
tempting to "fix" that to `qio_opi` because the module is an N16R8 and R8 usually
means octal. On this board that silently kills PSRAM:

| `memory_type` | `ESP.getPsramSize()` |
| --- | --- |
| `qio_qspi` | 8386231 |
| `qio_opi` | 0 |

With PSRAM dead the 150 KB frame buffer falls back to internal RAM, which still
boots but leaves too little heap for the Wi-Fi stack and HTTP server later.

The boot log prints `psram=` on every start specifically so this is visible
without a debugger.

## Wi-Fi will not connect

Two independent causes, both seen on this project:

**1. The SSID is 5 GHz.** The ESP32-S3 radio is 2.4 GHz only. A network that
works fine on a laptop may be invisible to the device. On a KT GiGA router the
5 GHz network is named `KT_GiGA_5G_XXXX` and the 2.4 GHz one is `KT_GiGA_XXXX`
— note that the 2.4 GHz name has no band marker at all, so `_2G_` is not a
reliable thing to look for.

**2. SSID case.** SSIDs are case-sensitive. `kt_giga_38bb` and `KT_GiGA_38BB`
are different networks, and the wrong one simply times out after 15 s with no
specific error.

Do not guess at either of these. The firmware runs a one-off 2.4 GHz scan at
boot and logs everything it can actually reach:

```
[WIFI] Scan found 6 network(s) reachable on 2.4 GHz:
[WIFI]   "KT_GiGA_38BB" ch=5 rssi=-48 auth=4
[WIFI]   "KT_GiGA_7360" ch=3 rssi=-82 auth=4
```

Copy the SSID from that list into `include/secrets.h` exactly as printed.
`auth` values follow the ESP-IDF `wifi_auth_mode_t` enum: 0 open, 2 WPA-PSK,
3 WPA2-PSK, 4 WPA/WPA2-PSK, 6 WPA3-PSK.

A Windows scan is not a substitute — `netsh wlan show networks` returned only
the single 5 GHz network the laptop was already associated with, while the
device saw six.

## The board is not detected for upload

Check the port before assuming a cable fault:

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like "*VID_303A*" }
```

The CoreS3 enumerates as `VID_303A & PID_1001` (the ESP32-S3 built-in
USB-Serial/JTAG). The board definition lists `303A:8119`, so PlatformIO's
auto-detection can miss it — pass the port explicitly:

```powershell
pio run -t upload --upload-port COM6
```

If nothing appears at all: confirm the USB-C cable carries data, then hold reset
for ~2 s to enter download mode.

## Reading the serial log without an interactive monitor

`pio device monitor` is interactive, which is awkward in scripts. `tools/capture_serial.py`
resets the board and captures a fixed window, so the boot log is always included:

```powershell
python tools/capture_serial.py COM6 20
```
