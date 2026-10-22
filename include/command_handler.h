#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_state.h"

// HTTP command surface (spec section 8, contract in docs/protocol.md).
//
// This module owns the protocol: parsing, duplicate detection and staleness.
// It never touches the state machine - it records what the PC asked for and
// main.cpp decides what that means.
namespace commands {

enum class NoticeKind : uint8_t { INFO, STRETCH, MEAL };

constexpr size_t kNoticeTextSize = 160;

struct Pending {
  bool has_state = false;
  AppState state = AppState::NORMAL;

  bool has_alert = false;  // an accepted, non-duplicate, non-stale alert

  bool has_mute = false;
  uint32_t mute_ms = 0;  // 0 means "cancel the current mute"

  bool has_test = false;  // local test alert, ignores mute

  // A spoken notice from the PC (stretch break, meal time). The PC plays the
  // audio; the device shows the same message so the two stay in sync.
  bool has_notice = false;
  char notice_text[kNoticeTextSize] = {0};
  NoticeKind notice_kind = NoticeKind::INFO;
  uint32_t notice_ms = 8000;
  bool notice_chime = true;
};

void begin();

// Call once per loop(). Services the web server; never blocks.
void update();

// Moves everything received since the last call into *out.
// Returns false when nothing was pending.
bool drain(Pending* out);

// Milliseconds since the PC last made any request. UINT32_MAX if never.
uint32_t msSinceLastContact();
bool everContacted();

// Mirrored back through GET /health.
void setReportedState(AppState state, bool muted, uint32_t mute_remaining_ms);

}  // namespace commands
