"""Posture monitor - the PC half of the posture-stackchan project.

Watches the webcam, scores forward-head posture against a calibrated baseline,
and drives the CoreS3 over HTTP. Also tracks how long you have been sitting and
speaks stretch and meal reminders.

    cd pc
    python monitor.py --preview

Keys in the preview window:
  q  quit
  c  recalibrate the neutral posture
  m  mute the device for 10 minutes
  t  fire a stretch reminder now, to test the voice
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from datetime import datetime
from pathlib import Path

import cv2

import config as config_module
from device import Device
from posture import PostureAnalyzer, PostureSample, ensure_model
from reminders import MealSchedule, SittingTimer
from voice import Voice

log = logging.getLogger("monitor")

MODEL_PATH = Path(__file__).with_name("models") / "pose_landmarker_lite.task"
BASELINE_PATH = Path(__file__).with_name("baseline.json")

# Largest time step the posture accumulator will honour in one go.
MAX_TICK_S = 1.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="CoreS3 posture monitor")
    parser.add_argument("--config", type=Path, default=None,
                        help="path to config.json (default: pc/config.json)")
    parser.add_argument("--device", help="override device_url, e.g. http://172.30.1.61")
    parser.add_argument("--camera", type=int, help="override camera index")
    parser.add_argument("--preview", action="store_true",
                        help="show the camera window with the live score")
    parser.add_argument("--no-voice", action="store_true", help="disable speech")
    parser.add_argument("--calibrate", action="store_true",
                        help="recalibrate instead of reusing the stored baseline")
    parser.add_argument("--verbose", action="store_true",
                        help="trace the score every 2 s instead of every 30 s")
    return parser.parse_args()


class PostureState:
    """Turns a stream of scores into NORMAL / BAD_PENDING / alert decisions.

    Edge-triggered timing ("bad for 5 seconds straight") is too brittle for a
    signal that flickers across the threshold: one good frame resets the clock,
    so alerts fire almost at random while the face flips every few seconds.

    This integrates instead. Bad time accumulates, good time drains it, and
    scores between the two thresholds hold the level steady. Brief flickers
    neither trigger nor reset, and genuinely sustained slouching always crosses
    the line.
    """

    def __init__(self, settings: dict) -> None:
        self.bad_score = settings["bad_score"]
        self.good_score = settings["good_score"]
        self.pending_after_s = settings["pending_after_s"]
        self.alert_after_s = settings["alert_after_s"]
        self.alert_repeat_s = settings["alert_repeat_s"]
        self.alert_min_gap_s = settings["alert_min_gap_s"]
        self.decay_rate = settings["decay_rate"]

        self.state = "NORMAL"
        self.bad_seconds = 0.0
        self._last_update: float | None = None
        # None, not 0.0: the repeat window must not suppress the very first
        # alert just because the clock happens to start near zero.
        self._last_alert_at: float | None = None
        self._recovered_at: float | None = None
        self._alert_counter = 0

    def _may_alert(self, now: float) -> bool:
        if self._last_alert_at is None:
            return True

        since_alert = now - self._last_alert_at
        if since_alert >= self.alert_repeat_s:
            return True

        # Straightening up and slouching again is a new episode, not the same
        # one continuing. alert_repeat_s exists to stop nagging every few
        # seconds through one long slouch - it should not swallow the feedback
        # for someone who actually corrected and then re-offended.
        recovered_since_alert = (self._recovered_at is not None and
                                 self._recovered_at > self._last_alert_at)
        return recovered_since_alert and since_alert >= self.alert_min_gap_s

    @property
    def bad_duration_ms(self) -> int:
        return int(self.bad_seconds * 1000)

    def update(self, score: float | None, now: float) -> tuple[str, bool]:
        """Returns (state, should_alert). `score` is None when nobody is seen."""
        delta = 0.0 if self._last_update is None else max(0.0, now - self._last_update)
        # A tracking dropout or a stall must not dump its whole gap into the
        # accumulator the moment tracking resumes.
        delta = min(delta, MAX_TICK_S)
        self._last_update = now

        if score is None:
            # Nobody in frame: hold the level rather than judging an empty chair.
            return self.state, False

        if score >= self.bad_score:
            self.bad_seconds += delta
        elif score <= self.good_score:
            self.bad_seconds -= delta * self.decay_rate

        # Cap the accumulator so a long slouch cannot bank credit that keeps the
        # warning up for minutes after you have already straightened.
        self.bad_seconds = max(0.0, min(self.bad_seconds, self.alert_after_s * 2.0))

        should_alert = False
        if self.bad_seconds >= self.alert_after_s:
            if self._may_alert(now):
                self._last_alert_at = now
                should_alert = True
            self.state = "ALERT" if should_alert else "BAD_PENDING"
        elif self.bad_seconds >= self.pending_after_s:
            self.state = "BAD_PENDING"
        elif self.bad_seconds <= 0.1:
            if self.state != "NORMAL":
                self._recovered_at = now
            self.state = "NORMAL"

        return self.state, should_alert

    def next_alert_id(self) -> str:
        self._alert_counter += 1
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        return f"{stamp}-{self._alert_counter:03d}"


def draw_preview(frame, sample: PostureSample, posture: "PostureState",
                 seated_minutes: int, calibrating: bool) -> None:
    height, width = frame.shape[:2]
    colour = {
        "NORMAL": (0, 200, 0),
        "BAD_PENDING": (0, 200, 220),
        "ALERT": (0, 0, 230),
    }.get(posture.state, (200, 200, 200))

    if calibrating:
        text = "CALIBRATING - sit normally"
        colour = (255, 200, 0)
    elif not sample.present:
        text = "no person detected"
        colour = (150, 150, 150)
    else:
        text = f"{posture.state}  score {sample.score:.2f}  [{sample.view}]"

    cv2.putText(frame, text, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, colour, 2)

    if sample.present:
        bar_width = int(sample.score * (width - 24))
        cv2.rectangle(frame, (12, 44), (12 + bar_width, 58), colour, -1)

        # Threshold markers, so it is obvious how close the score is to firing.
        for level, mark in ((posture.good_score, (120, 120, 120)),
                            (posture.bad_score, (0, 0, 230))):
            x = 12 + int(level * (width - 24))
            cv2.line(frame, (x, 40), (x, 62), mark, 1)

        # The accumulator is what actually decides, so show it filling up.
        fill = min(1.0, posture.bad_seconds / posture.alert_after_s)
        cv2.rectangle(frame, (12, 66), (12 + int(fill * (width - 24)), 74),
                      (0, 0, 230) if fill >= 1.0 else (0, 160, 220), -1)
        cv2.putText(frame, f"bad {posture.bad_seconds:.1f}s / {posture.alert_after_s:.0f}s",
                    (12, 92), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (220, 220, 220), 1)

    cv2.putText(frame, f"seated {seated_minutes} min", (12, height - 16),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (220, 220, 220), 1)


def main() -> int:
    args = parse_args()
    # Kept at INFO even for --verbose: DEBUG turns on pages of comtypes and
    # TensorFlow chatter that bury our own output.
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    cfg = config_module.load(args.config)
    if args.device:
        cfg["device_url"] = args.device
    if args.camera is not None:
        cfg["camera_index"] = args.camera
    if args.preview:
        cfg["preview"] = True
    if args.no_voice:
        cfg["voice"]["enabled"] = False

    posture_cfg = cfg["posture"]
    sitting_cfg = cfg["sitting"]
    messages = cfg["messages"]

    device = Device(cfg["device_url"])
    voice = Voice(**cfg["voice"])
    sitting = SittingTimer(sitting_cfg["stretch_after_min"],
                           sitting_cfg["away_resets_after_min"],
                           sitting_cfg["stretch_repeat_min"])
    meals = MealSchedule(cfg["meals"])

    health = device.health()
    if health:
        log.info("device %s is up (state %s)", cfg["device_url"], health.get("state"))
    else:
        log.warning("device %s did not answer - continuing anyway",
                    cfg["device_url"])

    analyzer = PostureAnalyzer(ensure_model(MODEL_PATH), posture_cfg["smoothing"],
                               posture_cfg["view"])

    capture = cv2.VideoCapture(cfg["camera_index"], cv2.CAP_DSHOW)
    if not capture.isOpened():
        log.error("camera %s could not be opened", cfg["camera_index"])
        return 1

    posture = PostureState(posture_cfg)

    # Reuse the stored baseline so a normal launch starts judging immediately.
    # Recalibrating on every run is not just slow: start the program while
    # already slouching and that slouch silently becomes "neutral".
    calibration_deadline = 0.0
    if args.calibrate or not analyzer.load_baseline(BASELINE_PATH):
        analyzer.start_calibration()
        calibration_deadline = time.monotonic() + posture_cfg["calibration_seconds"]

    frame_interval = 1.0 / max(1, cfg["target_fps"])
    started = time.monotonic()
    last_heartbeat = 0.0
    last_sent_state: str | None = None
    last_state_sent_at = 0.0
    min_state_interval = posture_cfg["min_state_interval_s"]
    last_trace = 0.0
    trace_interval = 2.0 if args.verbose else 30.0
    exit_code = 0

    log.info("running - Ctrl+C to stop")

    try:
        while True:
            loop_started = time.monotonic()

            ok, frame = capture.read()
            if not ok:
                log.warning("dropped frame")
                time.sleep(0.1)
                continue

            now = time.monotonic()
            timestamp_ms = int((now - started) * 1000)
            sample = analyzer.process(frame, timestamp_ms)

            if analyzer.calibrating:
                if now >= calibration_deadline:
                    if analyzer.finish_calibration():
                        analyzer.save_baseline(BASELINE_PATH)
                        voice.say(messages["calibrated"])
                        device.notify("자세 기준을 잡았어요", kind="info",
                                      duration_ms=4000, chime=True)
                    else:
                        log.warning("retrying calibration - stay in frame")
                        analyzer.start_calibration()
                        calibration_deadline = now + posture_cfg["calibration_seconds"]
            else:
                sitting.update(sample.present, now)
                state, should_alert = posture.update(
                    sample.score if sample.present else None, now)

                if sample.present:
                    if should_alert:
                        result = device.alert(
                            posture.next_alert_id(),
                            reason="forward_head",
                            score=sample.score,
                            duration_ms=posture.bad_duration_ms,
                            timestamp_ms=int(time.time() * 1000),
                        )
                        log.info("alert sent (score %.2f) -> %s",
                                 sample.score, result)
                        if cfg["speak_posture_alerts"]:
                            voice.say(messages["posture_alert"])
                        last_sent_state = "ALERT"
                        last_state_sent_at = now

                    elif state != last_sent_state and \
                            (now - last_state_sent_at) >= min_state_interval:
                        device.set_state(state, sample.score,
                                         posture.bad_duration_ms,
                                         int(time.time() * 1000))
                        log.info("state -> %s (score %.2f, bad %.1fs, %s view)",
                                 state, sample.score, posture.bad_seconds,
                                 sample.view)
                        last_sent_state = state
                        last_state_sent_at = now

                if sitting.should_remind(now):
                    minutes = sitting.seated_minutes
                    text = messages["stretch"].format(minutes=minutes)
                    log.info("stretch reminder at %d minutes", minutes)
                    voice.say(text)
                    device.notify(f"{minutes}분째 앉아 있어요\n일어나서 스트레칭!",
                                  kind="stretch", duration_ms=10000)
                    sitting.mark_reminded(now)

                meal = meals.due(datetime.now())
                if meal:
                    log.info("meal reminder: %s", meal["name"])
                    voice.say(meal["text"])
                    device.notify(meal["text"], kind="meal", duration_ms=10000)

            # Periodic trace. Without it a long run says nothing between state
            # changes, and "no output" reads the same as "not working".
            if now - last_trace >= trace_interval:
                last_trace = now
                if analyzer.calibrating:
                    log.info("calibrating ...")
                else:
                    log.info("score %.2f  bad %.1fs  %s  %s  raw %.2f/%.2f  "
                             "seated %d min",
                             sample.score, posture.bad_seconds, posture.state,
                             sample.view if sample.present else "no person",
                             sample.primary, sample.secondary,
                             sitting.seated_minutes)

            # Heartbeat keeps the device out of its OFFLINE face.
            if now - last_heartbeat >= 10.0:
                device.health()
                last_heartbeat = now

            if cfg["preview"]:
                draw_preview(frame, sample, posture, sitting.seated_minutes,
                             analyzer.calibrating)
                cv2.imshow("posture-stackchan", frame)
                key = cv2.waitKey(1) & 0xFF
                if key == ord("q"):
                    break
                if key == ord("c"):
                    analyzer.start_calibration()
                    calibration_deadline = now + posture_cfg["calibration_seconds"]
                if key == ord("m"):
                    device.mute(10 * 60 * 1000)
                    log.info("device muted for 10 minutes")
                if key == ord("t"):
                    # Fire a stretch reminder now, so the voice and the device
                    # notice can be checked without waiting 45 minutes.
                    text = messages["stretch"].format(
                        minutes=sitting.seated_minutes)
                    log.info("test stretch reminder")
                    voice.say(text)
                    device.notify("스트레칭 테스트\n일어나서 목이랑 어깨를 펴 주세요",
                                  kind="stretch", duration_ms=8000)

            elapsed = time.monotonic() - loop_started
            if elapsed < frame_interval:
                time.sleep(frame_interval - elapsed)

    except KeyboardInterrupt:
        log.info("stopping")
    except Exception:
        log.exception("monitor crashed")
        exit_code = 1
    finally:
        capture.release()
        if cfg["preview"]:
            cv2.destroyAllWindows()
        analyzer.close()
        voice.stop()

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
