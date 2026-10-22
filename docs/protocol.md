# PC ⇄ CoreS3 protocol

Transport for v1 is plain HTTP: the **CoreS3 runs the server**, the PC posture
detector is the client. MQTT may be added or swapped in later once HTTP is stable.

> Not implemented yet — this is the contract stage 3 will implement, published
> early so the PC-side program can be written in parallel.

Base URL: `http://<CORE_S3_IP>/` (port 80). All bodies are `application/json`.

## Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/health` | liveness + heartbeat; resets the offline timer |
| POST | `/state` | report the current posture state |
| POST | `/alert` | fire an audible alert |
| POST | `/mute` | mute remotely |
| POST | `/notify` | show a spoken notice (stretch break, meal time) |
| POST | `/settings` | change volume, brightness, mute length, buzzer on/off |
| POST | `/test` | trigger the local test alert |

### `GET /health`

```json
{
  "device": "posture-stackchan",
  "state": "NORMAL",
  "uptime_ms": 3821000,
  "muted": false,
  "sound_enabled": true
}
```

The PC should poll this at least every 10 s. If the CoreS3 sees no request from
the PC for **30 s**, it switches the display to `OFFLINE` while keeping the last
known posture state internally.

### `POST /state`

```json
{
  "state": "BAD_PENDING",
  "score": 0.72,
  "duration_ms": 3200,
  "timestamp": 1785460000000
}
```

`state` must be one of `NORMAL`, `BAD_PENDING`, `ALERT`. `score` and
`duration_ms` are informational. Changing state never makes sound; only
`/alert` does.

### `POST /alert`

```json
{
  "alert_id": "20260731-124300-001",
  "reason": "forward_head",
  "score": 0.86,
  "duration_ms": 6200,
  "expires_in_ms": 5000
}
```

Rules:

- **Duplicates** — an `alert_id` already seen is acknowledged but does not beep again.
- **Stale requests** — if the request arrives more than `expires_in_ms` after it
  was created, it is dropped. This keeps a request queued behind a Wi-Fi
  reconnect from firing minutes late.
- **Muted** — while muted the alert is recorded but silent.

Responses: `202` fired, `200` accepted-but-suppressed (duplicate/stale/muted),
`400` malformed.

### `POST /mute`

```json
{ "duration_ms": 600000 }
```

Omitting `duration_ms` uses the stored default of 10 minutes. `{"duration_ms": 0}`
cancels an active mute.

### `POST /settings`

```json
{ "volume": 60, "brightness": 140, "mute_duration_ms": 600000, "sound_enabled": true }
```

Every field is optional; only the ones present are changed. Values are clamped,
applied immediately and persisted to NVS, and the full resulting settings are
returned. The same values are also adjustable by hand on the device's settings
screen (tap the top-right corner).

Volume and brightness are 0-255. `mute_duration_ms` is clamped to 1-60 minutes.

Responses: `200` with the new settings, `400` if a value is out of range or no
known field was given.

### `POST /notify`

```json
{
  "text": "45분째 앉아 있어요\n일어나서 스트레칭!",
  "kind": "stretch",
  "duration_ms": 10000,
  "chime": true
}
```

Shows the message on the device for `duration_ms`, then returns to whatever
posture face was showing. `kind` is `stretch` (green bar), `meal` (orange) or
`info` (cyan). `chime` plays a rising two-tone that is deliberately different
from the posture alert.

`text` is UTF-8 and may be Korean; it is wrapped to the screen width, never
splitting a character, up to four lines. `\n` forces a line break. A touch
dismisses the notice early.

**The device does not speak.** The PC plays the audio through its own speakers
and posts the same words here so the screen and the voice stay in sync.

Responses: `200` shown, `400` when `text` is missing.

### `POST /test`

Empty body. Fires the alert pattern regardless of posture state; ignores mute.
Used for the stage-3 acceptance check.

## Quick check from the PC

```bash
curl -X POST http://CORE_S3_IP/alert \
  -H "Content-Type: application/json" \
  -d "{\"alert_id\":\"test-001\",\"reason\":\"test\",\"expires_in_ms\":5000}"
```

Expected: the face turns angry, two short beeps play, and the serial log shows
`[STATE] NORMAL -> ALERT`.

## Posture scoring stays on the PC

The CoreS3 renders state and makes noise. Landmark extraction, calibration
against the user's neutral posture, the forward-head score and the "bad for more
than 5 s" decision all live in the PC program.
