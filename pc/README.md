# PC posture monitor

The PC half of posture-stackchan. Watches the webcam, scores forward-head
posture, and drives the CoreS3 over HTTP. Also tracks how long you have been
sitting and speaks stretch and meal reminders.

**All judgement happens here.** The CoreS3 only renders what it is told.

## Setup

```powershell
cd pc
pip install -r requirements.txt
copy config.example.json config.json
```

Edit `config.json` and set `device_url` to the IP the device shows under its
face. `config.json` is git-ignored.

The pose model (~5 MB) downloads itself into `pc/models/` on first run.

## Run

```powershell
python monitor.py             # headless
python monitor.py --preview   # with the camera window
```

Keys in the preview window: `q` quit, `c` recalibrate, `m` mute the device for
10 minutes, `t` fire a stretch reminder immediately (to hear the voice without
waiting 45 minutes).

To start it automatically at logon (no console window, log in
`pc/logs/monitor.log`, restarted after 30 s if it exits with an error):

```powershell
powershell -ExecutionPolicy Bypass -File ..\tools\install_autostart.ps1            # install
powershell -ExecutionPolicy Bypass -File ..\tools\install_autostart.ps1 -Uninstall # remove
```

Useful flags: `--calibrate` (ignore the stored baseline), `--device http://…`,
`--camera 1`, `--no-voice`, `--verbose` (trace the score every 2 s).

## Camera placement

Both placements work, but they are **not** equally good.

**Side** (camera to your left or right, roughly head height) is the accurate
one. Forward head posture is clinically measured from a lateral view, and only
from the side can "head pushed forward" be told apart from "head tilted down to
look at the keyboard". Prefer it if you can spare the position.

**Front** (above the monitor) is the convenient one. It works, but a forward
head and a downward glance shorten the apparent neck identically, so expect it
to be twitchier and to need threshold tuning.

`posture.view` is `auto` by default: the view is decided from the shoulder
separation during calibration and then **locked for the session**. Locking
matters — re-deciding per frame lets the classification flap near the
threshold, and a frame classified differently from the baseline has nothing to
compare against, so the score silently sits at zero and everything looks fine
while nothing works. Set `"view": "front"` or `"side"` to pin it.

## How posture is scored

MediaPipe 1.0 removed the legacy `mp.solutions` API, so this uses the Tasks
`PoseLandmarker`. Do not downgrade below mediapipe 1.0 without rewriting
`posture.py`.

Scoring is **relative to you**, not to an absolute ideal. The first
`calibration_seconds` of the session measure your neutral posture, and every
later frame is compared against that baseline. Sit the way you actually want to
sit while it calibrates; press `c` to redo it.

Front view:

| Signal | Meaning | Weight |
| --- | --- | --- |
| neck ratio | ear-to-shoulder vertical distance ÷ shoulder width | 0.6 |
| face scale | ear-to-ear distance ÷ shoulder width; grows as you lean in | 0.4 |

Side view uses a single signal: **how many degrees the ear-over-shoulder line
tips away from vertical**, a craniovertebral-angle proxy. Beyond
`SIDE_ANGLE_FULL_DEG` (12°) above your calibrated baseline the score is 1.0.

Measured on a real desk: neutral sitting reads 22–30°, a deliberate forward
head reads 36–46°. Clean separation, no threshold guessing needed.

Two earlier attempts at the side metric failed in ways worth not repeating:

- Dividing the ear-shoulder offset by head length made the reading swing
  wildly, because the projected nose-to-ear distance changes as soon as the
  head rotates. An angle needs no scale reference at all.
- Inferring the facing direction from the nose flipped sign between frames near
  head-on, which drove the forward offset negative and clamped the score to
  zero *exactly* when the head was pushed forward. Using `|dx|` removes the
  question entirely, and works whether the camera is on your left or right.

Readings where the ear sits within `MIN_NECK_DY` of shoulder height are
discarded: `atan2` is hypersensitive there, and the shoulder landmark is
already the jitteriest part of a profile view. The angle is then smoothed
before scoring — smoothing only the final score is not enough, because the
score saturates at 1.0 and a single wild frame becomes indistinguishable from a
genuine slouch once clipped.

Front measurements are normalised by shoulder width, so sitting closer to the
desk does not by itself count as bad posture.

## Calibration is stored

The baseline is written to `pc/baseline.json` and reused on the next launch, so
a normal start begins judging immediately.

This is not only about saving eight seconds. Recalibrating on every run means
that starting the program while already slouching quietly makes that slouch the
definition of "neutral", and nothing you do afterwards registers.

Recalibrate with `--calibrate`, or `c` in the preview window, **whenever the
camera moves**. The log prints how old the stored baseline is on every start.

## Why bad posture is timed by integration

The obvious rule — "bad for 5 seconds straight" — is too brittle for a signal
that flickers across the threshold. A single good frame resets the clock, so
alerts fire almost at random while the device face flips every few seconds.
That is exactly what the first version did: 12 state changes in three minutes.

Instead bad time accumulates and good time drains it (`decay_rate`), and scores
between the two thresholds hold the level steady. Measured on synthetic signals
at 10 fps over 60 s:

| Input | State changes | Alerts |
| --- | --- | --- |
| good posture | 0 | 0 |
| noise straddling the threshold | 1 | 0 |
| 20 s slouch, then recovery | 4 | 1, at 5.2 s |
| continuous slouch | 3 | 1, then every `alert_repeat_s` |

A genuine square wave (2 s bad, 2 s good, repeating) still changes state often,
because that really is someone alternating posture every two seconds. No alert
fires, and `min_state_interval_s` throttles how fast the device face follows.

```
bad_seconds >= pending_after_s  ->  POST /state BAD_PENDING
bad_seconds >= alert_after_s    ->  POST /alert
bad_seconds back to ~0          ->  POST /state NORMAL
```

## Why the thresholds are so insensitive

The first settings judged posture over about five seconds. Across a real seven
hour session that produced **192 alerts** — one every two minutes.

Replaying the recorded angles explains why. Over those seven hours the measured
angle spanned p10 = 10.8° to p90 = 48.8°, and even a two-minute rolling median
still spanned 25°. The baseline captured at calibration sat almost exactly on
the median, so the signal spent half its time above any threshold placed near
it.

The cause is inherent to one camera: the ear-to-shoulder offset moves when the
head **turns** — toward a second monitor, the keyboard, the CoreS3 — not only
when it moves forward. Over a working day that happens constantly. Smoothing
does not help, because this is real variation rather than per-frame noise.

So the design accepts the limit: judge only clearly sustained slouching, and
accept missing brief ones. `tune_from_log.py` replays a recorded log against
candidate settings so this is chosen from evidence rather than intuition:

```powershell
python tune_from_log.py path\to\monitor.log --baseline 30.62
```

Same seven hour recording, alerts produced:

| Setting | Alerts | Per hour |
| --- | --- | --- |
| original (5 s) | 210 | 30.0 |
| 10 s hold | 130 | 18.5 |
| 15 s hold, stricter | 57 | 8.1 |
| **45 s sustained (current)** | **8** | **1.1** |
| 60 s sustained | 4 | 0.6 |

If it still nags too much, run the tool against your own log and raise
`alert_after_s`. If it misses slouches you care about, lower it — but check the
alert count before trusting a lower number.

## How often it is allowed to nag

Two different limits, because "still slouching" and "slouched again" deserve
different treatment:

| Situation | Limit |
| --- | --- |
| one long slouch, never corrected | `alert_repeat_s` (60 s) |
| straightened up, then slouched again | `alert_min_gap_s` (15 s) |

A single repeat window handles the first case but botches the second: correct
your posture, slouch again a moment later, and the alert is swallowed because
the 60 seconds have not elapsed. You did the right thing and got no feedback
for re-offending. An episode counts as over only when `bad_seconds` drains back
to zero, so briefly sitting up mid-slouch does not buy a fresh alert.

## Sitting timer

Time accrues only while the camera actually sees you. Being away for
`away_resets_after_min` counts as the break itself, so the timer restarts on
its own and you are not nagged the moment you sit back down.

After `stretch_after_min` of accumulated sitting you get a spoken reminder plus
a notice on the device, repeated every `stretch_repeat_min` until you get up.

## Meal reminders

Each entry in `meals` fires once a day, but only within 15 minutes of its
scheduled time. That window is deliberate: starting the program at 15:00 must
not immediately announce breakfast and lunch.

## Voice

Speech uses the Windows SAPI voices through pyttsx3, on a worker thread so the
capture loop never waits for a sentence to finish.

A Korean voice is picked automatically when one is installed — this machine has
**Microsoft Heami**. If none is found the program logs the installed voices and
falls back to the default; add one under Settings → Time & language → Speech.

Posture alerts are silent by default (`speak_posture_alerts: false`) and use the
device buzzer only. Being told to sit up straight out loud every minute gets
grating fast; stretch and meal reminders are the ones worth speaking.

## Files

| File | Role |
| --- | --- |
| `monitor.py` | main loop, posture state machine, entry point |
| `posture.py` | landmarks → score, calibration, model download |
| `device.py` | CoreS3 HTTP client; every call is best-effort |
| `voice.py` | TTS worker thread |
| `reminders.py` | sitting timer and meal schedule |
| `config.py` | config loading with defaults |

The device being unplugged, rebooting or off the network never stops the
monitor — `device.py` logs one warning per outage and keeps going.
