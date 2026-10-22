#pragma once

#include <stdint.h>

// Non-blocking alert tones on the CoreS3 internal speaker.
namespace audio {

void begin();

// Alert pattern from spec section 6: 200 ms tone, 150 ms silence, 200 ms tone.
// Returns immediately; the sequence is driven by update().
// Ignored while a pattern is already playing.
void playAlertPattern();

// A friendlier two-tone rising chime for notices, so a stretch reminder does
// not sound like a posture warning.
void playChime();

// Call once per loop().
void update();

bool isPlaying();

// Master switch for the buzzer. Muting goes through here.
void setEnabled(bool enabled);
bool isEnabled();

void setVolume(uint8_t volume);

}  // namespace audio
