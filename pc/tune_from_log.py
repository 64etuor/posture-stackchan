"""Replay a recorded monitor log through the scorer to compare settings.

Threshold tuning by intuition produced 192 alerts in seven hours. This replays
the angles that actually happened against candidate settings so the choice is
made on evidence.

    python tune_from_log.py <monitor.log> --baseline 30.62
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from monitor import PostureState

# 15:33:06 [monitor] score 0.01  bad 0.0s  NORMAL  side  raw 31.77/0.15  seated 0 min
LINE = re.compile(
    r"^(\d\d):(\d\d):(\d\d).*?score\s+[\d.]+\s+bad\s+[\d.]+s\s+\S+\s+(\S+)\s+"
    r"raw\s+([\d.]+)/([\d.]+)")

BASE_SETTINGS = {
    "bad_score": 0.55,
    "good_score": 0.35,
    "pending_after_s": 2.0,
    "alert_after_s": 5.0,
    "alert_repeat_s": 60.0,
    "alert_min_gap_s": 15.0,
    "decay_rate": 1.0,
}

CANDIDATES = [
    ("current", dict(alert_after_s=5.0, pending_after_s=2.0, decay_rate=1.0,
                     bad_score=0.55), 12.0),
    ("longer hold", dict(alert_after_s=10.0, pending_after_s=4.0,
                         decay_rate=1.5, bad_score=0.55), 12.0),
    ("longer + stricter", dict(alert_after_s=10.0, pending_after_s=4.0,
                               decay_rate=1.5, bad_score=0.60), 18.0),
    ("strictest", dict(alert_after_s=15.0, pending_after_s=5.0,
                       decay_rate=2.0, bad_score=0.65), 18.0),
    ("sustained 30s", dict(alert_after_s=30.0, pending_after_s=10.0,
                           decay_rate=2.0, bad_score=0.70,
                           alert_repeat_s=600.0, alert_min_gap_s=300.0), 20.0),
    ("sustained 45s", dict(alert_after_s=45.0, pending_after_s=15.0,
                           decay_rate=2.0, bad_score=0.70,
                           alert_repeat_s=600.0, alert_min_gap_s=300.0), 20.0),
    ("sustained 60s", dict(alert_after_s=60.0, pending_after_s=20.0,
                           decay_rate=2.5, bad_score=0.75,
                           alert_repeat_s=900.0, alert_min_gap_s=300.0), 20.0),
]


def read_samples(path: Path) -> list[tuple[float, float | None]]:
    """Returns (seconds_since_start, angle or None when nobody was seen)."""
    samples: list[tuple[float, float | None]] = []
    start: float | None = None

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(line)
        if not match:
            continue

        hours, minutes, seconds, view, angle, dy = match.groups()
        stamp = int(hours) * 3600 + int(minutes) * 60 + int(seconds)
        if start is None:
            start = stamp
        elapsed = stamp - start
        if elapsed < 0:
            elapsed += 24 * 3600

        absent = view == "person" or (float(angle) == 0.0 and float(dy) == 0.0)
        samples.append((float(elapsed), None if absent else float(angle)))

    return samples


def replay(samples, settings: dict, full_deg: float, baseline: float) -> int:
    state = PostureState(settings)
    alerts = 0

    for index, (stamp, angle) in enumerate(samples):
        # Hold each reading until the next one, stepping at the real loop rate
        # so the accumulator sees realistic deltas.
        next_stamp = samples[index + 1][0] if index + 1 < len(samples) else stamp
        span = max(0.1, min(next_stamp - stamp, 5.0))

        score = None
        if angle is not None:
            score = max(0.0, min(1.0, (angle - baseline) / full_deg))

        step = stamp
        while step < stamp + span:
            _, fired = state.update(score, step)
            if fired:
                alerts += 1
            step += 0.1

    return alerts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--baseline", type=float, required=True)
    args = parser.parse_args()

    samples = read_samples(args.log)
    if not samples:
        print("no usable score lines found - was the log written with --verbose?")
        return 1

    span_hours = (samples[-1][0] - samples[0][0]) / 3600.0
    seen = sum(1 for _, angle in samples if angle is not None)
    print(f"{len(samples)} samples over {span_hours:.1f} h "
          f"({seen} with a person in frame), baseline {args.baseline:.2f} deg\n")

    print(f"{'setting':20} {'alerts':>7} {'per hour':>9}")
    for name, overrides, full_deg in CANDIDATES:
        settings = {**BASE_SETTINGS, **overrides}
        alerts = replay(samples, settings, full_deg, args.baseline)
        print(f"{name:20} {alerts:7d} {alerts / max(span_hours, 1e-6):9.1f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
