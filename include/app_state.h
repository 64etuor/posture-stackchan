#pragma once

#include <stdint.h>

// Application states from spec section 5.1.
//
// Note: the spec calls the last state ERROR, but ERROR is a macro in several
// ESP32 / Arduino headers, so the enumerator is named FAULT. appStateName()
// still reports it as "ERROR" in logs and on screen.
enum class AppState : uint8_t {
  BOOTING = 0,
  CONNECTING,
  NORMAL,
  BAD_PENDING,
  ALERT,
  MUTED,
  OFFLINE,
  FAULT,

  // Not in the original spec list: a transient spoken notice pushed by the PC
  // (stretch break, meal time). It overlays the posture states the way ALERT
  // does and always falls back to whatever the posture state was.
  NOTICE,
};

const char* appStateName(AppState state);

// All state changes go through this one object so that transitions are
// logged and timed in a single place (spec section 5.1).
class StateMachine {
 public:
  void begin(AppState initial);

  // Returns true when the state actually changed. Re-entering the same state
  // is a no-op and is not logged, so callers may call this from the loop.
  bool transitionTo(AppState next, const char* reason);

  AppState state() const { return state_; }
  AppState previousState() const { return previous_; }

  // Milliseconds since the current state was entered.
  uint32_t elapsedMs() const;

 private:
  AppState state_ = AppState::BOOTING;
  AppState previous_ = AppState::BOOTING;
  uint32_t entered_at_ms_ = 0;
};
