"""Windows text-to-speech on a worker thread.

pyttsx3's runAndWait() blocks until the phrase finishes, and its engine is not
safe to share across threads, so the engine is created inside the worker and
fed through a queue. The capture loop never waits on speech.
"""

from __future__ import annotations

import logging
import queue
import threading

log = logging.getLogger(__name__)

_STOP = object()


class Voice:
    def __init__(self, enabled: bool = True, rate: int = 170, volume: float = 0.9,
                 voice_hint: str = "korean") -> None:
        self.enabled = enabled
        self._rate = rate
        self._volume = volume
        self._hint = voice_hint.lower()
        self._queue: queue.Queue = queue.Queue()
        self._thread: threading.Thread | None = None

        if not enabled:
            log.info("voice disabled by config")
            return

        self._thread = threading.Thread(target=self._run, name="voice", daemon=True)
        self._thread.start()

    def say(self, text: str) -> None:
        if not self.enabled or not text:
            return
        self._queue.put(text)

    def stop(self) -> None:
        if self._thread is None:
            return
        self._queue.put(_STOP)
        self._thread.join(timeout=5)

    # ------------------------------------------------------------------ worker

    def _pick_voice(self, engine) -> None:
        """Prefers a Korean voice when one is installed."""
        try:
            voices = engine.getProperty("voices")
        except Exception as error:  # pragma: no cover - driver specific
            log.warning("could not enumerate voices: %s", error)
            return

        for voice in voices:
            haystack = f"{getattr(voice, 'name', '')} {getattr(voice, 'id', '')}".lower()
            languages = getattr(voice, "languages", []) or []
            language_text = " ".join(str(item).lower() for item in languages)
            if self._hint in haystack or "ko-kr" in haystack or "ko_kr" in language_text:
                engine.setProperty("voice", voice.id)
                log.info("voice: %s", getattr(voice, "name", voice.id))
                return

        names = [getattr(v, "name", "?") for v in voices]
        log.warning(
            "no Korean voice found, using the default. Installed: %s. "
            "Add one via Settings > Time & language > Speech.", names)

    def _run(self) -> None:
        try:
            import pyttsx3
        except ImportError:
            log.error("pyttsx3 is not installed; voice output is off")
            self.enabled = False
            return

        try:
            engine = pyttsx3.init()
        except Exception as error:  # pragma: no cover - driver specific
            log.error("could not start the speech engine: %s", error)
            self.enabled = False
            return

        engine.setProperty("rate", self._rate)
        engine.setProperty("volume", self._volume)
        self._pick_voice(engine)

        while True:
            item = self._queue.get()
            if item is _STOP:
                break
            try:
                log.info("say: %s", item)
                engine.say(item)
                engine.runAndWait()
            except Exception as error:  # pragma: no cover - driver specific
                log.warning("speech failed: %s", error)

        try:
            engine.stop()
        except Exception:
            pass
