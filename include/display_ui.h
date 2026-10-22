#pragma once

#include "app_state.h"

// Face + status rendering. Knows nothing about networking or the state
// machine's transition rules; it only renders the state it is told about.
namespace ui {

void begin();

// Switch the rendered expression. Cheap to call repeatedly with the same
// value; a redraw is only scheduled when something actually changes.
void setState(AppState state);

// Small line of text under the face (IP address, mute countdown, hints).
// Pass nullptr or "" to clear it.
void setSubText(const char* text);

// Call once per loop(). Drives the blink animation and pushes the frame
// buffer only when the image changed. Never blocks.
void update();

// Force a redraw after something the UI reads (a setting) changed underneath it.
void markDirty();

// Content shown while the state is AppState::NOTICE. `text` may be UTF-8
// Korean; it is wrapped to the screen width. `label` is the bar caption and
// should stay ASCII.
void setNotice(const char* text, const char* label, uint16_t bar_color);

enum class Screen : uint8_t { FACE, SETTINGS };

void setScreen(Screen screen);
Screen screen();

// What a touch at (x, y) means on the settings screen. The layout lives in
// display_ui.cpp, so hit testing belongs here too.
enum class SettingsAction : uint8_t {
  NONE,
  VOLUME_DOWN,
  VOLUME_UP,
  BRIGHTNESS_DOWN,
  BRIGHTNESS_UP,
  MUTE_DOWN,
  MUTE_UP,
  SOUND_OFF,
  SOUND_ON,
  CLOSE,
};

SettingsAction hitTestSettings(int16_t x, int16_t y);

// True when (x, y) is the top-right corner that opens the settings screen.
bool hitTestSettingsCorner(int16_t x, int16_t y);

}  // namespace ui
