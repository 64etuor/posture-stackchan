#include "settings.h"

#include <M5Unified.h>
#include <Preferences.h>

#include "audio.h"
#include "logging.h"

namespace settings {
namespace {

constexpr char kNamespace[] = "posture";

// Limits kept away from the extremes: a brightness of 0 looks like a dead
// device, and a mute longer than an hour defeats the point of the alert.
constexpr uint8_t kMinBrightness = 20;
constexpr uint32_t kMinMuteMs = 60UL * 1000UL;
constexpr uint32_t kMaxMuteMs = 60UL * 60UL * 1000UL;

Preferences prefs;

Values values = {
    // 120 was uncomfortably loud on a desk 50 cm away.
    /* volume */ 60,
    /* brightness */ 140,
    /* mute_duration_ms */ 10UL * 60UL * 1000UL,
    /* sound_enabled */ true,
};

uint32_t clampMute(uint32_t ms) {
  if (ms < kMinMuteMs) return kMinMuteMs;
  if (ms > kMaxMuteMs) return kMaxMuteMs;
  return ms;
}

}  // namespace

void begin() {
  prefs.begin(kNamespace, false);

  values.volume = prefs.getUChar("volume", values.volume);
  values.brightness = prefs.getUChar("bright", values.brightness);
  values.mute_duration_ms = clampMute(prefs.getUInt("mutems", values.mute_duration_ms));
  values.sound_enabled = prefs.getBool("sound", values.sound_enabled);

  if (values.brightness < kMinBrightness) {
    values.brightness = kMinBrightness;
  }

  audio::setVolume(values.volume);
  audio::setEnabled(values.sound_enabled);
  M5.Display.setBrightness(values.brightness);

  LOG_SETTINGS("loaded volume=%u brightness=%u mute=%lus sound=%d",
               static_cast<unsigned>(values.volume),
               static_cast<unsigned>(values.brightness),
               static_cast<unsigned long>(values.mute_duration_ms / 1000),
               values.sound_enabled ? 1 : 0);
}

const Values& get() { return values; }

void setVolume(uint8_t volume) {
  values.volume = volume;
  audio::setVolume(volume);
  prefs.putUChar("volume", volume);
  LOG_SETTINGS("volume=%u", static_cast<unsigned>(volume));
}

void setBrightness(uint8_t brightness) {
  values.brightness = (brightness < kMinBrightness) ? kMinBrightness : brightness;
  M5.Display.setBrightness(values.brightness);
  prefs.putUChar("bright", values.brightness);
  LOG_SETTINGS("brightness=%u", static_cast<unsigned>(values.brightness));
}

void setMuteDurationMs(uint32_t duration_ms) {
  values.mute_duration_ms = clampMute(duration_ms);
  prefs.putUInt("mutems", values.mute_duration_ms);
  LOG_SETTINGS("mute duration=%lus",
               static_cast<unsigned long>(values.mute_duration_ms / 1000));
}

void setSoundEnabled(bool enabled) {
  values.sound_enabled = enabled;
  audio::setEnabled(enabled);
  prefs.putBool("sound", enabled);
  LOG_SETTINGS("sound=%d", enabled ? 1 : 0);
}

}  // namespace settings
