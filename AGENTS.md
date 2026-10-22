# Working rules for this repository

Firmware for an M5Stack CoreS3 (K128) posture alert device. Read `README.md` for
what the current build does and `docs/protocol.md` for the PC-facing contract.

## Before writing code

Look at the existing files first. The module split is deliberate — `main.cpp`
wires things together and owns no feature logic.

## Hard rules

1. **Never guess an API.** Use M5Unified / M5GFX calls that are known to exist.
   If unsure, check the installed library source under `.pio/libdeps/` rather
   than inventing a signature.
2. **Never guess pin numbers or power configuration.** Use the CoreS3 built-in
   peripherals through M5Unified.
3. **Pin every dependency** in `platformio.ini`. No floating version ranges.
4. **No long blocking calls.** `millis()`-based timing only; the animation, the
   beep sequencer, networking and the mute timer must not stall each other.
   The single `delay(5)` at the end of `loop()` is the only permitted delay.
5. **All state changes go through `StateMachine::transitionTo()`.** It is the
   only place that logs `[STATE] A -> B`.
6. **No secrets in git.** Credentials live in `include/secrets.h`, which is
   git-ignored. Never log a password or token.
7. **No servos or external parts** until the user asks for them.
8. **One stage at a time.** Each change must be verifiable on real hardware.
   Keep diffs small and do not break behaviour that already works.

## Logging

Use the macros in `include/logging.h` so the format stays `[TAG] message`.
Every meaningful event should be visible on the serial monitor at 115200.

## Report format after each task

```
변경 내용     - what was implemented
수정 파일     - files touched
실행 방법     - build and upload commands
확인 방법     - what to see on screen, what to hear, what the serial log shows
문제 발생 시  - which logs and settings to check
```

## Verify before claiming done

`pio run` must succeed. State clearly what was verified by compilation versus
what still needs a check on the physical device — screen output, sound and touch
cannot be confirmed from a build alone.
