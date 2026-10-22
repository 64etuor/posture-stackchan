"""Frames from the CoreS3 built-in camera, pulled over HTTP.

Duck-types the parts of cv2.VideoCapture the monitor uses (isOpened, read,
release), so the posture loop does not care where frames come from.

The device serves its newest JPEG at GET /capture and returns 503 while the
camera warms up after an idle period - that is a dropped frame, not an error.
"""

from __future__ import annotations

import logging

import cv2
import numpy as np
import requests

log = logging.getLogger(__name__)


class DeviceCamera:
    def __init__(self, device_url: str, timeout: float = 2.0) -> None:
        self.url = device_url.rstrip("/") + "/capture"
        self.timeout = timeout
        self._session = requests.Session()
        self._failing = False

    def isOpened(self) -> bool:  # noqa: N802 - mirrors cv2.VideoCapture
        # The device may still be booting; read() copes with that, so never
        # refuse to start over it.
        return True

    def read(self) -> tuple[bool, np.ndarray | None]:
        try:
            response = self._session.get(self.url, timeout=self.timeout)
        except requests.RequestException as error:
            self._fail(f"unreachable: {error}")
            return False, None

        if response.status_code != 200:
            self._fail(f"HTTP {response.status_code}")
            return False, None

        frame = cv2.imdecode(np.frombuffer(response.content, np.uint8),
                             cv2.IMREAD_COLOR)
        if frame is None:
            self._fail("undecodable JPEG")
            return False, None

        if self._failing:
            log.info("device camera frames resumed")
            self._failing = False
        return True, frame

    def release(self) -> None:
        self._session.close()

    def _fail(self, reason: str) -> None:
        # Once per outage, not once per frame.
        if not self._failing:
            log.warning("device camera %s: %s", self.url, reason)
            self._failing = True
