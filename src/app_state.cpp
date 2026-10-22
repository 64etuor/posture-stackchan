#include "app_state.h"

#include <Arduino.h>

#include "logging.h"

const char* appStateName(AppState state) {
  switch (state) {
    case AppState::BOOTING:
      return "BOOTING";
    case AppState::CONNECTING:
      return "CONNECTING";
    case AppState::NORMAL:
      return "NORMAL";
    case AppState::BAD_PENDING:
      return "BAD_PENDING";
    case AppState::ALERT:
      return "ALERT";
    case AppState::MUTED:
      return "MUTED";
    case AppState::OFFLINE:
      return "OFFLINE";
    case AppState::FAULT:
      return "ERROR";
    case AppState::NOTICE:
      return "NOTICE";
  }
  return "UNKNOWN";
}

void StateMachine::begin(AppState initial) {
  state_ = initial;
  previous_ = initial;
  entered_at_ms_ = millis();
  LOG_STATE("init -> %s", appStateName(state_));
}

bool StateMachine::transitionTo(AppState next, const char* reason) {
  if (next == state_) {
    return false;
  }
  previous_ = state_;
  state_ = next;
  entered_at_ms_ = millis();
  LOG_STATE("%s -> %s (%s)", appStateName(previous_), appStateName(state_),
            reason ? reason : "-");
  return true;
}

uint32_t StateMachine::elapsedMs() const {
  // Unsigned subtraction stays correct across the 49-day millis() rollover.
  return millis() - entered_at_ms_;
}
