"""Forward-head ("turtle neck") scoring from a webcam.

MediaPipe 1.0 removed the legacy `mp.solutions` API, so this uses the Tasks
PoseLandmarker, which needs a model file downloaded once and cached.

Two camera placements are supported, and they are not equally good:

**side** (camera to your left or right, roughly at head height) is the accurate
one. Forward head posture is clinically measured from a lateral view, because
only from the side can you separate "head translated forward" from "head tilted
down to look at the keyboard". Prefer this if you can spare the camera position.

**front** (camera above the monitor) is the convenient one. It works, but it
cannot distinguish a forward head from a downward glance - both shorten the
apparent neck the same way. Expect it to be twitchier.

`view="auto"` picks per frame from how far apart the shoulders are horizontally.

Scoring is relative in both modes: a neutral posture is measured during
calibration and every later frame is compared against that baseline.
"""

from __future__ import annotations

import json
import logging
import math
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions
from mediapipe.tasks.python import vision

log = logging.getLogger(__name__)

MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/pose_landmarker/"
    "pose_landmarker_lite/float16/1/pose_landmarker_lite.task"
)

# How far each signal must move from the baseline to count as fully bad.
FRONT_NECK_DROP_FULL = 0.30
FRONT_LEAN_IN_FULL = 0.25
FRONT_NECK_WEIGHT = 0.6
FRONT_LEAN_WEIGHT = 0.4

# Degrees the ear-over-shoulder line has to tip away from vertical, beyond the
# calibrated baseline, to count as a fully forward head.
SIDE_ANGLE_FULL_DEG = 20.0

# The shoulder landmark is jittery in profile, and the angle explodes when the
# ear sits close to shoulder height, so readings below this vertical separation
# are thrown away rather than trusted.
MIN_NECK_DY = 0.06

# The raw angle is smoothed before it is scored. Smoothing only the final score
# is not enough: the score saturates at 1.0, so a single wild frame is
# indistinguishable from a genuine slouch once it has been clipped.
ANGLE_SMOOTHING = 0.2

# Bump when the stored fields change meaning, so old files are discarded
# instead of silently misinterpreted.
BASELINE_VERSION = 1

MIN_VISIBILITY = 0.5

# Shoulders further apart than this (fraction of frame width) means we are
# looking at someone head-on rather than side-on.
FRONT_SHOULDER_SEPARATION = 0.16


@dataclass
class PostureSample:
    present: bool
    score: float = 0.0
    view: str = "-"
    primary: float = 0.0    # neck ratio (front) or forward offset (side)
    secondary: float = 0.0  # face scale (front) or neck length (side)


def ensure_model(path: Path) -> Path:
    """Downloads the pose model on first run and caches it next to the code."""
    if path.exists():
        return path

    path.parent.mkdir(parents=True, exist_ok=True)
    log.info("downloading pose model to %s (about 5 MB, one time)", path)
    urllib.request.urlretrieve(MODEL_URL, path)
    log.info("model ready")
    return path


def _visible(mark) -> bool:
    return getattr(mark, "visibility", 1.0) >= MIN_VISIBILITY


class PostureAnalyzer:
    def __init__(self, model_path: Path, smoothing: float = 0.12,
                 view: str = "auto") -> None:
        options = vision.PoseLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=str(model_path)),
            running_mode=vision.RunningMode.VIDEO,
            num_poses=1,
            min_pose_detection_confidence=0.5,
            min_pose_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        self._landmarker = vision.PoseLandmarker.create_from_options(options)
        self._smoothing = smoothing
        self._view_mode = view

        # Baselines are kept per view so switching does not mix them up.
        self._baseline: dict[str, tuple[float, float]] = {}
        self._calibrating = False
        self._samples: dict[str, list[tuple[float, float]]] = {}
        self._smoothed = 0.0

        # Auto-detection is decided once during calibration and then locked.
        # Re-deciding per frame lets the view flap near the threshold, and a
        # frame classified differently from the baseline has nothing to compare
        # against - the score silently sits at zero.
        self._locked_view: str | None = None
        self._warned_no_baseline = False
        self._angle_ema: float | None = None

    # ------------------------------------------------------------ calibration

    def start_calibration(self) -> None:
        self._calibrating = True
        self._samples = {}
        self._locked_view = None
        self._angle_ema = None
        log.info("calibrating - sit the way you want to sit")

    @property
    def calibrating(self) -> bool:
        return self._calibrating

    @property
    def calibrated(self) -> bool:
        return bool(self._baseline)

    def finish_calibration(self) -> bool:
        """Turns the collected samples into a baseline. False if too few."""
        self._calibrating = False

        best_view = max(self._samples, key=lambda v: len(self._samples[v]),
                        default=None)
        if best_view is None or len(self._samples[best_view]) < 10:
            count = len(self._samples.get(best_view, [])) if best_view else 0
            log.warning("calibration failed: only %d usable frames", count)
            return False

        samples = self._samples[best_view]
        primary = float(np.median([s[0] for s in samples]))
        secondary = float(np.median([s[1] for s in samples]))
        self._baseline[best_view] = (primary, secondary)
        self._locked_view = best_view

        tally = ", ".join(f"{view} {len(rows)}"
                          for view, rows in sorted(self._samples.items()))
        log.info("baseline [%s view] primary=%.3f secondary=%.3f "
                 "from %d frames (classified: %s)",
                 best_view, primary, secondary, len(samples), tally)
        if len(self._samples) > 1:
            log.info("view locked to %s for this session; set posture.view "
                     "explicitly in config.json to override", best_view)

        self._samples = {}
        return True

    # ------------------------------------------------------------ persistence

    def save_baseline(self, path: Path) -> None:
        if not self._baseline or self._locked_view is None:
            return
        primary, secondary = self._baseline[self._locked_view]
        payload = {
            "version": BASELINE_VERSION,
            "view": self._locked_view,
            "primary": primary,
            "secondary": secondary,
            "saved_at": time.time(),
        }
        path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        log.info("baseline saved to %s", path.name)

    def load_baseline(self, path: Path) -> bool:
        """Restores a stored baseline so every launch does not recalibrate."""
        if not path.exists():
            return False

        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (ValueError, OSError) as error:
            log.warning("could not read %s: %s", path.name, error)
            return False

        if data.get("version") != BASELINE_VERSION:
            log.info("stored baseline is an older format - recalibrating")
            return False

        view = data.get("view")
        if view not in ("front", "side"):
            log.info("stored baseline has no usable view - recalibrating")
            return False

        if self._view_mode in ("front", "side") and view != self._view_mode:
            log.info("stored baseline is for the %s view but config asks for %s"
                     " - recalibrating", view, self._view_mode)
            return False

        self._baseline[view] = (float(data["primary"]), float(data["secondary"]))
        self._locked_view = view

        age_hours = (time.time() - float(data.get("saved_at", 0.0))) / 3600.0
        log.info("using stored %s baseline (%.1f h old, primary=%.2f). "
                 "Recalibrate with --calibrate, or c in the preview window, "
                 "whenever the camera moves.", view, age_hours, data["primary"])
        return True

    # ------------------------------------------------------------ measurement

    def _measure_front(self, marks) -> tuple[float, float] | None:
        left_ear = marks[vision.PoseLandmark.LEFT_EAR]
        right_ear = marks[vision.PoseLandmark.RIGHT_EAR]
        left_shoulder = marks[vision.PoseLandmark.LEFT_SHOULDER]
        right_shoulder = marks[vision.PoseLandmark.RIGHT_SHOULDER]

        if not all(_visible(m) for m in
                   (left_ear, right_ear, left_shoulder, right_shoulder)):
            return None

        shoulder_width = abs(left_shoulder.x - right_shoulder.x)
        if shoulder_width < 1e-3:
            return None

        ear_mid_y = (left_ear.y + right_ear.y) / 2.0
        shoulder_mid_y = (left_shoulder.y + right_shoulder.y) / 2.0

        neck_ratio = (shoulder_mid_y - ear_mid_y) / shoulder_width
        face_scale = abs(left_ear.x - right_ear.x) / shoulder_width
        return neck_ratio, face_scale

    def _measure_side(self, marks, aspect: float) -> tuple[float, float] | None:
        """Craniovertebral-angle proxy: how far the ear leans off vertical.

        Deliberately uses |dx|, so it does not matter which way the person
        faces or which side the camera is on. An earlier version inferred the
        facing direction from the nose; near head-on that sign flipped between
        frames, turning the forward offset negative and clamping the score to
        zero exactly when the head *was* pushed forward.

        There is also no scale reference to go wrong - an angle is already
        dimensionless, whereas dividing by head length made the reading swing
        whenever the head rotated.
        """
        pairs = (
            (marks[vision.PoseLandmark.LEFT_EAR],
             marks[vision.PoseLandmark.LEFT_SHOULDER]),
            (marks[vision.PoseLandmark.RIGHT_EAR],
             marks[vision.PoseLandmark.RIGHT_SHOULDER]),
        )
        candidates = [(ear, shoulder) for ear, shoulder in pairs
                      if _visible(ear) and _visible(shoulder)]
        if not candidates:
            return None

        ear, shoulder = max(
            candidates,
            key=lambda pair: getattr(pair[0], "visibility", 1.0) +
                             getattr(pair[1], "visibility", 1.0))

        # Landmarks are normalised per axis, so x must be rescaled by the frame
        # aspect before the two can be mixed in an angle.
        dx = abs(ear.x - shoulder.x) * aspect
        dy = shoulder.y - ear.y
        if dy < MIN_NECK_DY:
            # Ear at or near shoulder height: atan2 is hypersensitive here and
            # the reading would be mostly noise.
            return None

        forward_angle = math.degrees(math.atan2(dx, dy))

        if self._angle_ema is None:
            self._angle_ema = forward_angle
        else:
            self._angle_ema += ANGLE_SMOOTHING * (forward_angle - self._angle_ema)

        return self._angle_ema, dy

    def _pick_view(self, marks) -> str:
        if self._view_mode in ("front", "side"):
            return self._view_mode
        if self._locked_view is not None:
            return self._locked_view

        left_shoulder = marks[vision.PoseLandmark.LEFT_SHOULDER]
        right_shoulder = marks[vision.PoseLandmark.RIGHT_SHOULDER]
        if _visible(left_shoulder) and _visible(right_shoulder):
            if abs(left_shoulder.x - right_shoulder.x) >= FRONT_SHOULDER_SEPARATION:
                return "front"
        return "side"

    # --------------------------------------------------------------- analysis

    def process(self, frame_bgr, timestamp_ms: int) -> PostureSample:
        rgb = frame_bgr[:, :, ::-1]
        image = mp.Image(image_format=mp.ImageFormat.SRGB,
                         data=np.ascontiguousarray(rgb))
        result = self._landmarker.detect_for_video(image, timestamp_ms)

        if not result.pose_landmarks:
            return PostureSample(present=False)

        height, width = frame_bgr.shape[:2]
        marks = result.pose_landmarks[0]
        view = self._pick_view(marks)
        measured = (self._measure_front(marks) if view == "front"
                    else self._measure_side(marks, width / height))
        if measured is None:
            return PostureSample(present=False, view=view)

        primary, secondary = measured

        if self._calibrating:
            self._samples.setdefault(view, []).append((primary, secondary))
            return PostureSample(present=True, view=view, primary=primary,
                                 secondary=secondary)

        baseline = self._baseline.get(view)
        if baseline is None:
            # Scoring zero forever looks exactly like "posture is perfect", so
            # say out loud that there is nothing to compare against.
            if not self._warned_no_baseline:
                self._warned_no_baseline = True
                log.warning("no baseline for the %s view - recalibrate (press c)",
                            view)
            return PostureSample(present=True, view=view, primary=primary,
                                 secondary=secondary)

        base_primary, base_secondary = baseline

        if view == "front":
            # Neck shortens, face grows as the head comes toward the screen.
            neck_drop = max(0.0, (base_primary - primary) / abs(base_primary))
            lean_in = max(0.0, (secondary - base_secondary) / abs(base_secondary))
            raw = (FRONT_NECK_WEIGHT * min(1.0, neck_drop / FRONT_NECK_DROP_FULL) +
                   FRONT_LEAN_WEIGHT * min(1.0, lean_in / FRONT_LEAN_IN_FULL))
        else:
            # One dimensionless, sign-stable signal: degrees of lean added on
            # top of however you normally sit.
            extra_degrees = max(0.0, primary - base_primary)
            raw = min(1.0, extra_degrees / SIDE_ANGLE_FULL_DEG)

        raw = max(0.0, min(1.0, raw))
        self._smoothed += self._smoothing * (raw - self._smoothed)

        return PostureSample(present=True, score=self._smoothed, view=view,
                             primary=primary, secondary=secondary)

    def close(self) -> None:
        self._landmarker.close()
