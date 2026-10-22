#pragma once

#include <stdint.h>

// Persistent user settings (spec section 10). Stored in NVS through
// Preferences so they survive a reboot.
namespace settings {

struct Values {
  uint8_t volume;             // speaker volume, 0-255
  uint8_t brightness;         // display backlight, 0-255
  uint32_t mute_duration_ms;  // how long one mute lasts
  bool sound_enabled;         // master switch for the buzzer
};

// Loads stored values (or defaults) and applies them to the hardware.
// Call after ui::begin() and audio::begin().
void begin();

const Values& get();

// Setters clamp to a sane range, apply to the hardware and persist.
void setVolume(uint8_t volume);
void setBrightness(uint8_t brightness);
void setMuteDurationMs(uint32_t duration_ms);
void setSoundEnabled(bool enabled);

}  // namespace settings
