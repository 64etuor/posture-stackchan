"""Configuration loading for the posture monitor.

Values come from pc/config.json (copy pc/config.example.json). Anything the
file omits falls back to the defaults here, so an old config keeps working
when new options are added.
"""

from __future__ import annotations

import copy
import json
import logging
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

DEFAULTS: dict[str, Any] = {
    "device_url": "http://172.30.1.61",
    "camera_index": 0,
    "target_fps": 10,
    "posture": {
        # "side" is the accurate placement, "front" the convenient one,
        # "auto" decides per frame from the shoulder separation.
        "view": "auto",
        # How long to average a neutral posture before judging anything.
        "calibration_seconds": 8,
        # Hysteresis: it takes bad_score to go bad, good_score to recover.
        #
        # These are deliberately insensitive. Measured against a real 7 hour
        # session, the first settings fired 192 alerts - one every two minutes.
        # The single-camera angle simply is not clean enough to judge posture
        # over a few seconds, so only clearly sustained slouching counts. Same
        # recording, these settings: 8 alerts in 7 hours.
        "bad_score": 0.70,
        "good_score": 0.40,
        "pending_after_s": 15.0,
        "alert_after_s": 45.0,
        # Never nag more often than this while one slouch continues.
        "alert_repeat_s": 600.0,
        # Floor between alerts once you have straightened up in between, so a
        # corrected-then-repeated slouch still gets feedback.
        "alert_min_gap_s": 300.0,
        # Exponential moving average factor for the raw score, 0..1.
        # Lower is steadier; 0.12 at 10 fps is roughly a one second constant.
        "smoothing": 0.12,
        # How fast good posture drains the accumulated bad time. 2.0 means a
        # second of sitting up cancels two seconds of slouching, so brief
        # excursions do not creep toward the alert.
        "decay_rate": 2.0,
        # Floor on how often a state change is pushed to the device, so the
        # face cannot flicker even if the score is briefly indecisive.
        "min_state_interval_s": 3.0,
    },
    "sitting": {
        "stretch_after_min": 45,
        "away_resets_after_min": 3,
        "stretch_repeat_min": 10,
    },
    "meals": [
        {"name": "breakfast", "at": "08:00", "text": "아침 먹을 시간이에요."},
        {"name": "lunch", "at": "12:00", "text": "점심 시간이에요."},
        {"name": "dinner", "at": "18:30", "text": "저녁 시간이에요."},
    ],
    "voice": {
        "enabled": True,
        "rate": 170,
        "volume": 0.9,
        "voice_hint": "korean",
    },
    "messages": {
        "stretch": "{minutes}분째 앉아 있어요. 일어나서 목이랑 어깨를 펴 주세요.",
        "posture_alert": "거북목이에요. 허리를 펴고 턱을 당겨 주세요.",
        "calibrated": "자세 기준을 잡았어요. 이제부터 확인할게요.",
    },
    # Posture alerts normally use the device buzzer only; speaking every time
    # gets grating fast.
    "speak_posture_alerts": False,
    "preview": False,
}


def _merge(base: Any, override: Any) -> Any:
    if isinstance(base, dict) and isinstance(override, dict):
        merged = copy.deepcopy(base)
        for key, value in override.items():
            merged[key] = _merge(base.get(key), value) if key in base else value
        return merged
    return copy.deepcopy(override)


def load(path: Path | None = None) -> dict[str, Any]:
    """Reads config.json next to this file, falling back to defaults."""
    if path is None:
        path = Path(__file__).with_name("config.json")

    if not path.exists():
        log.warning("%s not found, using defaults", path.name)
        return copy.deepcopy(DEFAULTS)

    with path.open("r", encoding="utf-8") as handle:
        user = json.load(handle)

    log.info("loaded %s", path)
    return _merge(DEFAULTS, user)
