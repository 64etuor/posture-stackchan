"""Reset the CoreS3 and capture its serial log for a fixed time.

`pio device monitor` is interactive, which is awkward for scripted checks.
This opens the port, pulses the reset line so the boot log is captured from
the first line, then prints whatever arrives.

    python tools/capture_serial.py COM6 10
"""

import sys
import time

import serial

# The firmware logs UTF-8. Without this the Windows console decodes it as
# CP949 and Korean notice text looks corrupted even though the bytes are fine.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

with serial.Serial(port, 115200, timeout=0.2) as s:
    # ESP32-S3 USB-Serial/JTAG maps RTS to EN and DTR to IO0.
    # DTR low keeps IO0 high so the chip boots the application, not the ROM loader.
    s.setDTR(False)
    s.setRTS(True)   # EN low  -> held in reset
    time.sleep(0.2)
    s.setRTS(False)  # EN high -> boot
    s.reset_input_buffer()

    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = s.read(4096)
        if chunk:
            # The firmware terminates lines with CRLF; on Windows stdout the LF
            # would be expanded again and every line would gain a blank one.
            text = chunk.decode("utf-8", errors="replace").replace("\r\n", "\n")
            sys.stdout.write(text)
            sys.stdout.flush()

print("\n--- capture finished ---")
