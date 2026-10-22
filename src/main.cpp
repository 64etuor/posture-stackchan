// CoreS3 posture alert device - stages 1-4.
//
// boot -> Wi-Fi -> HTTP server. The PC decides posture and pushes state and
// alerts; this device renders them, makes noise, and handles mute and settings
// by touch. Posture is never judged here (spec section 12, stage 5).
//
// main.cpp only wires the modules together and owns the transition rules.
// Rendering, sound, Wi-Fi, the HTTP protocol and persistence each live in
// their own translation unit.

#include <M5Unified.h>

#include "app_state.h"
#include "audio.h"
#include "camera.h"
#include "command_handler.h"
#include "display_ui.h"
#include "logging.h"
#include "network.h"
#include "settings.h"

namespace {

constexpr uint32_t kBootScreenMs = 1200;
constexpr uint32_t kAlertDurationMs = 2000;

// No request from the PC for this long means it is gone (spec section 9).
constexpr uint32_t kPcTimeoutMs = 30000;

constexpr uint32_t kLongPressMs = 800;

constexpr uint8_t kVolumeStep = 20;
constexpr uint8_t kBrightnessStep = 20;
constexpr uint32_t kMuteStepMs = 60UL * 1000UL;

StateMachine sm;

// The posture the PC last reported. ALERT and MUTED are transient overlays on
// top of it, so we can restore the right face once they finish.
AppState reported_state = AppState::NORMAL;

// millis() deadline, or 0 when not muted.
uint32_t mute_deadline_ms = 0;

// millis() deadline for the notice currently on screen.
uint32_t notice_deadline_ms = 0;

uint32_t press_started_ms = 0;
bool long_press_fired = false;
bool server_started = false;

char sub_text[32] = {0};

bool isMuted() {
  return mute_deadline_ms != 0 &&
         static_cast<int32_t>(millis() - mute_deadline_ms) < 0;
}

uint32_t muteRemainingMs() { return isMuted() ? (mute_deadline_ms - millis()) : 0; }

void startMute(uint32_t duration_ms) {
  mute_deadline_ms = millis() + duration_ms;
  if (mute_deadline_ms == 0) {
    mute_deadline_ms = 1;  // 0 is the "not muted" sentinel
  }
  audio::setEnabled(false);
  LOG_STATE("muted for %lus", static_cast<unsigned long>(duration_ms / 1000));
}

void endMute(const char* reason) {
  mute_deadline_ms = 0;
  audio::setEnabled(settings::get().sound_enabled);
  LOG_STATE("mute cleared (%s)", reason);
}

bool pcOnline() {
  return commands::everContacted() &&
         commands::msSinceLastContact() < kPcTimeoutMs;
}

// Where to settle once a transient state finishes. Mute outranks connectivity
// because it is user-initiated and shows a countdown the user is waiting on.
AppState restingState() {
  if (isMuted()) {
    return AppState::MUTED;
  }
  if (!net::isConnected() || !pcOnline()) {
    return AppState::OFFLINE;
  }
  return reported_state;
}

void fireAlert(const char* reason) {
  sm.transitionTo(AppState::ALERT, reason);
  audio::playAlertPattern();
}

void applySettingsAction(ui::SettingsAction action) {
  const settings::Values& v = settings::get();

  switch (action) {
    case ui::SettingsAction::NONE:
      return;

    case ui::SettingsAction::CLOSE:
      ui::setScreen(ui::Screen::FACE);
      return;

    case ui::SettingsAction::VOLUME_DOWN:
      settings::setVolume(v.volume > kVolumeStep ? v.volume - kVolumeStep : 0);
      break;

    case ui::SettingsAction::VOLUME_UP:
      settings::setVolume(v.volume + kVolumeStep > 255 ? 255
                                                       : v.volume + kVolumeStep);
      break;

    case ui::SettingsAction::BRIGHTNESS_DOWN:
      settings::setBrightness(
          v.brightness > kBrightnessStep ? v.brightness - kBrightnessStep : 0);
      break;

    case ui::SettingsAction::BRIGHTNESS_UP:
      settings::setBrightness(v.brightness + kBrightnessStep > 255
                                  ? 255
                                  : v.brightness + kBrightnessStep);
      break;

    case ui::SettingsAction::MUTE_DOWN:
      settings::setMuteDurationMs(v.mute_duration_ms > kMuteStepMs
                                      ? v.mute_duration_ms - kMuteStepMs
                                      : 0);
      break;

    case ui::SettingsAction::MUTE_UP:
      settings::setMuteDurationMs(v.mute_duration_ms + kMuteStepMs);
      break;

    case ui::SettingsAction::SOUND_OFF:
      settings::setSoundEnabled(false);
      break;

    case ui::SettingsAction::SOUND_ON:
      settings::setSoundEnabled(true);
      // Turning the buzzer back on must not cancel a mute in progress.
      if (isMuted()) {
        audio::setEnabled(false);
      }
      break;
  }

  ui::markDirty();
}

void handleTouch() {
  const auto touch = M5.Touch.getDetail();

  if (touch.wasPressed()) {
    press_started_ms = millis();
    long_press_fired = false;
    LOG_TOUCH("pressed at x=%d y=%d", touch.x, touch.y);

    if (ui::screen() == ui::Screen::SETTINGS) {
      applySettingsAction(ui::hitTestSettings(touch.x, touch.y));
      return;
    }

    if (ui::hitTestSettingsCorner(touch.x, touch.y)) {
      ui::setScreen(ui::Screen::SETTINGS);
      return;
    }

    switch (sm.state()) {
      case AppState::ALERT:
        startMute(settings::get().mute_duration_ms);
        sm.transitionTo(AppState::MUTED, "touch mute");
        break;

      case AppState::MUTED:
        endMute("touch");
        sm.transitionTo(restingState(), "touch unmute");
        break;

      case AppState::NOTICE:
        sm.transitionTo(restingState(), "touch dismiss");
        break;

      default:
        break;
    }
    return;
  }

  if (ui::screen() == ui::Screen::SETTINGS) {
    return;
  }

  // Long press fires a local test alert. Allowed while OFFLINE as well, so the
  // hardware can be checked without the PC program running.
  if (touch.isPressed() && !long_press_fired &&
      millis() - press_started_ms >= kLongPressMs) {
    long_press_fired = true;

    if (sm.state() == AppState::NORMAL || sm.state() == AppState::BAD_PENDING ||
        sm.state() == AppState::OFFLINE) {
      LOG_TOUCH("long press - test alert");
      fireAlert("touch test");
    }
  }
}

void applyPendingCommands() {
  commands::Pending pending;
  if (!commands::drain(&pending)) {
    return;
  }

  if (pending.has_mute) {
    if (pending.mute_ms == 0) {
      endMute("remote");
      sm.transitionTo(restingState(), "remote unmute");
    } else {
      startMute(pending.mute_ms);
      sm.transitionTo(AppState::MUTED, "remote mute");
    }
  }

  if (pending.has_state) {
    reported_state = pending.state;
    // Do not yank the face out of a running alert, mute or notice.
    if (sm.state() != AppState::ALERT && sm.state() != AppState::MUTED &&
        sm.state() != AppState::NOTICE) {
      sm.transitionTo(restingState(), "pc state");
    }
  }

  if (pending.has_alert) {
    if (isMuted()) {
      LOG_STATE("alert suppressed: muted");
    } else {
      fireAlert("pc alert");
    }
  }

  if (pending.has_notice) {
    const char* label = "NOTICE";
    uint16_t color = 0x07FF;  // cyan
    if (pending.notice_kind == commands::NoticeKind::STRETCH) {
      label = "STRETCH";
      color = 0x07E0;  // green
    } else if (pending.notice_kind == commands::NoticeKind::MEAL) {
      label = "MEAL";
      color = 0xFD20;  // orange
    }

    ui::setNotice(pending.notice_text, label, color);
    notice_deadline_ms = millis() + pending.notice_ms;
    sm.transitionTo(AppState::NOTICE, "pc notice");

    if (pending.notice_chime) {
      audio::playChime();
    }
  }

  if (pending.has_test) {
    // /test deliberately overrides mute (docs/protocol.md).
    if (isMuted()) {
      endMute("test override");
    }
    fireAlert("pc test");
  }
}

void updateTimers() {
  switch (sm.state()) {
    case AppState::BOOTING:
      if (sm.elapsedMs() >= kBootScreenMs) {
        sm.transitionTo(AppState::CONNECTING, "boot complete");
      }
      break;

    case AppState::CONNECTING:
      if (net::isConnected()) {
        if (!server_started) {
          commands::begin();
          server_started = true;
        }
        sm.transitionTo(restingState(), "wifi connected");
      }
      break;

    case AppState::ALERT:
      if (sm.elapsedMs() >= kAlertDurationMs && !audio::isPlaying()) {
        sm.transitionTo(restingState(), "alert done");
      }
      break;

    case AppState::NOTICE:
      if (static_cast<int32_t>(millis() - notice_deadline_ms) >= 0) {
        sm.transitionTo(restingState(), "notice done");
      }
      break;

    case AppState::MUTED:
      if (!isMuted()) {
        if (mute_deadline_ms != 0) {
          endMute("expired");
        }
        sm.transitionTo(restingState(), "mute expired");
      }
      break;

    default: {
      // NORMAL, BAD_PENDING and OFFLINE simply follow connectivity.
      const AppState target = restingState();
      if (target != sm.state()) {
        sm.transitionTo(target, "connectivity");
      }
      break;
    }
  }
}

void updateSubText() {
  if (sm.state() == AppState::MUTED && isMuted()) {
    const uint32_t remaining_s = muteRemainingMs() / 1000UL;
    snprintf(sub_text, sizeof(sub_text), "MUTED %lu:%02lu",
             static_cast<unsigned long>(remaining_s / 60),
             static_cast<unsigned long>(remaining_s % 60));
  } else if (!net::isConnected()) {
    strncpy(sub_text, net::statusText(), sizeof(sub_text) - 1);
  } else if (!pcOnline()) {
    snprintf(sub_text, sizeof(sub_text), "%s  NO PC", net::ipAddress());
  } else {
    strncpy(sub_text, net::ipAddress(), sizeof(sub_text) - 1);
  }

  sub_text[sizeof(sub_text) - 1] = '\0';
  ui::setSubText(sub_text);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  M5.begin(cfg);

  Serial.begin(115200);
  LOG_BOOT("CoreS3 initialized");
  LOG_BOOT("heap=%u bytes psram=%u bytes",
           static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(ESP.getPsramSize()));

  ui::begin();
  audio::begin();
  camera::begin();
  settings::begin();
  net::begin();

  sm.begin(AppState::BOOTING);
  ui::setState(sm.state());
  ui::setSubText("posture-stackchan");
  ui::update();

  LOG_BOOT("setup complete");
}

void loop() {
  M5.update();
  net::update();
  if (server_started) {
    commands::update();
  }

  handleTouch();
  applyPendingCommands();
  updateTimers();

  audio::update();

  commands::setReportedState(sm.state(), isMuted(), muteRemainingMs());

  ui::setState(sm.state());
  if (sm.state() != AppState::BOOTING && sm.state() != AppState::NOTICE) {
    updateSubText();
  }
  ui::update();

  // Yield to the RTOS without introducing a long blocking delay.
  delay(5);
}
