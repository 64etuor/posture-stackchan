"""HTTP client for the CoreS3.

Every call is best-effort: the device being unplugged, rebooting or off the
network must never take the monitor down with it. Failures are logged once per
outage rather than once per frame.
"""

from __future__ import annotations

import logging
from typing import Any

import requests

log = logging.getLogger(__name__)


class Device:
    def __init__(self, base_url: str, timeout: float = 2.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._session = requests.Session()
        self._offline = False

    # ---------------------------------------------------------------- helpers

    def _request(self, method: str, path: str, payload: dict[str, Any] | None = None):
        url = f"{self.base_url}{path}"
        try:
            response = self._session.request(
                method, url, json=payload, timeout=self.timeout
            )
        except requests.RequestException as error:
            if not self._offline:
                log.warning("device unreachable (%s): %s", url, error)
                self._offline = True
            return None

        if self._offline:
            log.info("device reachable again")
            self._offline = False

        if response.status_code >= 400:
            log.warning("%s %s -> %s %s", method, path, response.status_code,
                        response.text.strip())
            return None

        try:
            return response.json()
        except ValueError:
            return {}

    @property
    def offline(self) -> bool:
        return self._offline

    # -------------------------------------------------------------- endpoints

    def health(self) -> dict[str, Any] | None:
        return self._request("GET", "/health")

    def set_state(self, state: str, score: float, duration_ms: int,
                  timestamp_ms: int) -> bool:
        result = self._request(
            "POST",
            "/state",
            {
                "state": state,
                "score": round(score, 3),
                "duration_ms": int(duration_ms),
                "timestamp": timestamp_ms,
            },
        )
        return result is not None

    def alert(self, alert_id: str, reason: str, score: float, duration_ms: int,
              timestamp_ms: int, expires_in_ms: int = 5000) -> str | None:
        result = self._request(
            "POST",
            "/alert",
            {
                "alert_id": alert_id,
                "reason": reason,
                "score": round(score, 3),
                "duration_ms": int(duration_ms),
                "timestamp": timestamp_ms,
                "expires_in_ms": expires_in_ms,
            },
        )
        if result is None:
            return None
        return result.get("result")

    def mute(self, duration_ms: int) -> bool:
        return self._request("POST", "/mute", {"duration_ms": duration_ms}) is not None

    def notify(self, text: str, kind: str = "info", duration_ms: int = 8000,
               chime: bool = True) -> bool:
        return self._request(
            "POST",
            "/notify",
            {
                "text": text,
                "kind": kind,
                "duration_ms": duration_ms,
                "chime": chime,
            },
        ) is not None
