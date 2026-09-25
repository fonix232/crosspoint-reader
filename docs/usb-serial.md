# USB serial commands

Send newline-terminated ASCII commands to the awake reader's USB serial port at
115200 baud, with DTR asserted. Diagnostic log lines may be interleaved with replies.
On native USB CDC and USB Serial/JTAG connections, this is a compatibility setting,
not a UART bit-rate limit. Raising it does not increase USB throughput.

| Command | Reply / behavior |
| --- | --- |
| `CMD:USB STATUS` | `USB:OFF`, `USB:WAITING`, `USB:CONNECTED`, `USB:EJECTED`, `USB:DISCONNECTED`, `USB:ERROR`, or `USB:UNSUPPORTED`. |
| `CMD:USB ON` | `USB:STARTING`, then enters USB Drive through the normal activity and storage handoff. If already active, reports its current state. Allocation failure reports `USB:ERROR START_FAILED`. |
| `CMD:USB OFF` | `USB:OFF` if inactive. If the host still owns the card, reports `USB:ERROR EJECT_REQUIRED`. Otherwise replies `USB:RESTARTING`, detaches storage and restarts to Home. |

Eject the volume through the host operating system before leaving USB Drive mode.
Host ejection normally makes the reader restart automatically; `OFF` is also useful
when waiting for a host. It never forces a mounted or errored device offline.
Commands are handled even while the main loop is restricted to USB Drive activity.
Boards without USB mass-storage support reply `USB:ERROR UNSUPPORTED`.

On macOS, a normal remote session is:

1. Send `CMD:USB ON` and wait for the volume to mount.
2. Read or copy files, then verify writes.
3. Run `diskutil eject /Volumes/CROSSP` (use the actual volume name).
4. Wait for the reader's serial port to return; `CMD:USB STATUS` should report `USB:OFF`.

The X4 Pro also supports `CMD:FW_INFO` and the validated update protocol implemented
by `scripts/usb_firmware_upload.py`. Exit USB Drive mode before uploading firmware.

## Unattended input and screen capture

`scripts/wired_debug.py` needs Python 3 and `pyserial`; screenshots use the Python
standard library to write PNG files. Specify `--port` when more than one reader
or ESP32 serial device is connected. The CLI asserts DTR and deasserts RTS before
opening; it does not intentionally reset the reader.

```sh
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 info
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 screenshot /tmp/screen.png
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 tap 200 20
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 swipe 10 300 300 300
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 button back
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 tap 200 300 --duration 800
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 reset
python scripts/wired_debug.py --port /dev/cu.usbmodem11401 usb status
```

`info` reports logical dimensions, orientation, touch/Home-key support, USB
storage state, and uptime. Touch coordinates use the same top-left origin and
orientation as the saved PNG. `tap --duration 800` is a long hold. `down x y`,
`move x y`, and `up` allow explicit dragging; button `down`/`up` allow simultaneous
buttons. Tap/swipe/click timing runs on the reader, independent of host scheduling.
An accepted reply means the input was queued for the next normal input update,
not that its resulting navigation/render has finished. The CLI waits the gesture
duration plus `--settle` seconds (default 0.6) after input. Screenshots additionally
wait for outstanding inputs, navigation, and scheduled renders to finish, with a
bounded timeout; they do not force an extra repaint.

Buttons are the HAL's physical lines: `back`, `confirm`, `left`, `right`, `up`,
`down`, and `power`. These virtual lines are also available on boards without all
those physical switches. Normal user remapping, power shortcuts, and long-press
behavior apply. `home` emulates the capacitive Home key only where present.
Touch is rejected on boards without touch. This injects at the application HAL;
it does not test electrical debounce or the touch controller itself.

Manual contacts expire after ten seconds if the host disappears. `reset` cancels
all synthetic contacts without synthesizing a tap or click. USB-mode commands and
firmware commands also clear them. A power event can put the device to sleep;
serial input cannot wake a sleeping or powered-off reader. Use one serial client
at a time, especially during firmware upload.

Screenshots capture the existing **1-bit framebuffer**, not the physical panel's
retained grayscale planes. Pixels are rotated into logical orientation. Capture
holds the render lock while streaming, allocates no second framebuffer, and does
not write to the SD card. The host verifies dimensions, offsets, length, and CRC32
before replacing the output file. Logs may occur between protocol lines.

### Wire protocol version 1

Send `CMD:DEBUG <id> <operation>`, where `<id>` is a positive decimal 31-bit
request identifier. Replies start `DBG:<id>`. IDs correlate replies with the
current request even when logs or older replies are present. Lines are ASCII and
newline terminated, shorter than the existing 128-byte receive limit.

| Operation | Behavior |
| --- | --- |
| `INFO` | `INFO 1 <width> <height> <orientation> <touch> <home-key> <usb-storage> <uptime-ms>` |
| `RESET` | Cancel synthetic contacts; `OK RESET`. Available during USB storage. |
| `BUTTON <name> DOWN` / `UP` | Press/release a physical HAL line; `OK`. Names uppercase. |
| `BUTTON <name> CLICK <ms>` | Timed press/release, 1–5000 ms. |
| `TOUCH DOWN <x> <y>` / `MOVE <x> <y>` / `UP` | Manual single-contact events. |
| `TOUCH TAP <x> <y> <ms>` | Stationary contact, 1–5000 ms. |
| `TOUCH SWIPE <x> <y> <end-x> <end-y> <ms>` | Interpolated movement, 1–700 ms. Must travel at least 60 native pixels to classify as a swipe. |
| `SCREENSHOT` | `SCREENSHOT <native-width> <native-height> <stride> <orientation> <size>`, then `DATA <offset> <hex>`, then `END <crc32>`. |

Orientation values are 0=portrait, 1=landscape clockwise, 2=portrait inverted,
3=landscape counterclockwise/native. Screenshot chunks contain at most 64 bytes;
bits run most-significant first, with 1 representing white. CRC32 is the standard
IEEE CRC of the complete native framebuffer bytes. `ERROR BUSY_RENDER` and
`ERROR INPUT_ACTIVE` are retryable; the CLI retries these within `--timeout`.
Other explicit errors include `ARGUMENTS`, `COORDINATES`, `BUTTON_STATE`,
`TOUCH_STATE`, `NO_TOUCH`, `NO_HOME_KEY`, `USB_STORAGE_ACTIVE`, `NO_FRAMEBUFFER`,
`SHORT_WRITE`, and `TRANSFER_TIMEOUT`. No input or screenshot runs while USB mass
storage owns the card. The older `CMD:SCREENSHOT` binary protocol is retained for
existing monitors; use the new protocol for unattended captures on varying boards.

### Power diagnostics

`python scripts/wired_debug.py --port PORT power` reads raw battery millivolts,
voltage-derived estimate, cached UI percentage, CPU MHz, Wi-Fi mode/connection,
USB presence, charger status, PaperS3 EPD rail enables and analog USB-detect
millivolts. POWER v2 adds USB millivolts; the host also accepts v1 replies. Unknown values are
`-1`. The command does not update battery smoothing or take a performance lock;
it returns `BUSY_RENDER` instead of sampling during a display refresh. USB powers
the device during sampling, so this is not a battery-current measurement.

PaperS3 now requests PMS150G board power-off after its sleep image, reading state
and storage are finalized when running on battery. USB retains a sleep fallback
because pulsing the latch with USB attached can reboot immediately.

### Persistent PaperS3 power timeline

PaperS3 writes `/.crosspoint/power.csv` once per minute while awake, plus
`boot`, `usb_plug`, `usb_unplug`, `sleep_request`, and `sleep_ready` records.
USB edges are checked every 250 ms in the normal application loop; rendering
can defer a record. USB storage mode suspends logging because the host owns
that filesystem. Power-off cannot log subsequent cable changes; the next boot
records the initial USB state. No periodic wake is scheduled for logging.

CSV columns include a random boot/session ID, UTC (0 if unavailable), uptime,
raw voltage, raw and cached UI percentages, USB voltage/presence, charge status,
CPU clock, Wi-Fi state, display rails, reader-open state, and reset reason.
These are voltage-based diagnostics, **not measured current or coulomb counts**.
Sampling does not advance the UI battery smoothing or raise CPU frequency.
Each record is flushed and closed. At 16 MiB logging stops with an error rather
than overwriting history. SD writes add a small unmeasured diagnostic overhead.

Send `CMD:POWERLOG` over serial to retrieve the CSV between `POWERLOG_BEGIN`
and `POWERLOG_END`; `POWERLOG_BUSY` means USB storage currently owns the card.
The file can also be copied through USB storage. Logs survive reboot and firmware
updates; retain the CSV when comparing unplugged test runs.

Host retrieval helper (requires pyserial):

```sh
python scripts/power_log.py --port /dev/cu.usbmodem… --output power.csv
```

From the 2026-09-30 idle-power firmware, `CMD:POWERLOG` retrieves
`/.crosspoint/power-v2.csv`. The original `power.csv` is retained unchanged.
The new CSV adds cumulative `idle_sleep_us` and `idle_sleep_count`; comparing
successive samples measures time spent in successful idle naps (including
entry/exit overhead), not current consumption. PaperS3 naps use 10 ms timer
wakeups only on battery, with Wi-Fi/Bluetooth off and rendering/job admission
quiescent. USB keeps the processor awake for debugging. Three fresh readings
below 3400 mV, 30 seconds apart, request normal saved-state shutdown even when
auto-sleep is Never. Plugging in or recovering voltage resets that debounce.
