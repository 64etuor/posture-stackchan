"""Sitting-time tracking and meal-time scheduling."""

from __future__ import annotations

import logging
from datetime import date, datetime, time, timedelta
from typing import Any

log = logging.getLogger(__name__)


class SittingTimer:
    """Accumulates how long the user has been at the desk.

    Time only accrues while the camera actually sees someone. Stepping away
    long enough counts as the break, so the timer restarts by itself and the
    reminder does not fire the moment they sit back down.
    """

    def __init__(self, remind_after_min: float, away_resets_after_min: float,
                 repeat_min: float) -> None:
        self._remind_after_s = remind_after_min * 60.0
        self._away_resets_after_s = away_resets_after_min * 60.0
        self._repeat_s = repeat_min * 60.0

        self._seated_s = 0.0
        self._away_s = 0.0
        self._last_tick: float | None = None
        self._last_reminded_at: float | None = None

    def update(self, present: bool, now: float) -> None:
        if self._last_tick is None:
            self._last_tick = now
            return

        delta = max(0.0, now - self._last_tick)
        self._last_tick = now

        if present:
            self._away_s = 0.0
            self._seated_s += delta
            return

        self._away_s += delta
        if self._away_s >= self._away_resets_after_s and self._seated_s > 0.0:
            log.info("away for %.0f min - sitting timer reset",
                     self._away_s / 60.0)
            self.reset()

    def reset(self) -> None:
        self._seated_s = 0.0
        self._last_reminded_at = None

    @property
    def seated_minutes(self) -> int:
        return int(self._seated_s // 60)

    def should_remind(self, now: float) -> bool:
        if self._seated_s < self._remind_after_s:
            return False
        if self._last_reminded_at is None:
            return True
        return (now - self._last_reminded_at) >= self._repeat_s

    def mark_reminded(self, now: float) -> None:
        self._last_reminded_at = now


class MealSchedule:
    """Fires each configured meal once a day, within a short window.

    The window matters: starting the program at 15:00 must not immediately
    announce breakfast and lunch.
    """

    def __init__(self, meals: list[dict[str, Any]], window_minutes: int = 15) -> None:
        self._meals: list[dict[str, Any]] = []
        self._window = timedelta(minutes=window_minutes)
        self._fired: dict[str, date] = {}

        for meal in meals:
            try:
                hour, minute = (int(part) for part in str(meal["at"]).split(":"))
                parsed = time(hour=hour, minute=minute)
            except (KeyError, ValueError):
                log.warning("skipping meal with a bad time: %r", meal)
                continue
            self._meals.append({
                "name": meal.get("name", parsed.strftime("%H:%M")),
                "time": parsed,
                "text": meal.get("text", "밥 먹을 시간이에요."),
            })

        log.info("meal reminders: %s",
                 ", ".join(f"{m['name']} {m['time'].strftime('%H:%M')}"
                           for m in self._meals) or "none")

    def due(self, now: datetime) -> dict[str, Any] | None:
        for meal in self._meals:
            scheduled = datetime.combine(now.date(), meal["time"])
            if not (scheduled <= now < scheduled + self._window):
                continue
            if self._fired.get(meal["name"]) == now.date():
                continue
            self._fired[meal["name"]] = now.date()
            return meal
        return None
