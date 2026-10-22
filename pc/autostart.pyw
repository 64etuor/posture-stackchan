"""Windowless launcher for monitor.py, run at logon by Task Scheduler.

pythonw has no console, so stdout/stderr are sent to logs/monitor.log
(overwritten on each launch). If the monitor exits with an error - typically
the webcam not being ready yet right after logon - it is restarted after a
pause. A clean exit (code 0) ends the launcher.

Install the logon task with tools/install_autostart.ps1.
"""

import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
RETRY_S = 30

log_dir = HERE / "logs"
log_dir.mkdir(exist_ok=True)
log_file = open(log_dir / "monitor.log", "w", encoding="utf-8", buffering=1)
sys.stdout = sys.stderr = log_file

sys.path.insert(0, str(HERE))
sys.argv = [str(HERE / "monitor.py")]

import monitor  # noqa: E402  (stdout must be redirected first)

while True:
    try:
        code = monitor.main()
    except Exception:
        traceback.print_exc()
        code = 1
    if code == 0:
        break
    print(f"[autostart] monitor exited with {code}; restarting in {RETRY_S} s", flush=True)
    time.sleep(RETRY_S)
