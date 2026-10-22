#include "audio.h"

#include <M5Unified.h>
#include <stddef.h>

#include "logging.h"

namespace audio {
namespace {

constexpr uint8_t kDefaultVolume = 120;  // 0-255

// A tone sequence. hz == 0 is a silent gap.
struct ToneStep {
  float hz;
  uint32_t ms;
};

// Spec section 6: 200 ms tone, 150 ms silence, 200 ms tone.
const ToneStep kAlertPattern[] = {{2000.0f, 200}, {0.0f, 150}, {2000.0f, 200}};

// Rising two-tone, clearly not a warning.
const ToneStep kChimePattern[] = {{880.0f, 120}, {0.0f, 60}, {1320.0f, 220}};

const ToneStep* sequence = nullptr;
size_t sequence_length = 0;
size_t sequence_index = 0;
uint32_t step_started_ms = 0;
const char* sequence_name = "";

bool enabled = true;

void playStep() {
  const ToneStep& step = sequence[sequence_index];
  if (step.hz > 0.0f) {
    // M5Unified's tone() is asynchronous: it queues the waveform and returns.
    M5.Speaker.tone(step.hz, step.ms);
  }
  step_started_ms = millis();
}

void startSequence(const ToneStep* steps, size_t length, const char* name) {
  if (!enabled) {
    LOG_AUDIO("%s suppressed (muted)", name);
    return;
  }
  if (sequence != nullptr) {
    return;  // already playing; do not overlap or restart
  }

  sequence = steps;
  sequence_length = length;
  sequence_index = 0;
  sequence_name = name;
  playStep();
  LOG_AUDIO("%s started", name);
}

}  // namespace

void begin() {
  // A safe starting point; settings::begin() applies the stored volume next.
  M5.Speaker.setVolume(kDefaultVolume);
  LOG_AUDIO("speaker ready (enabled=%d volume=%u)", M5.Speaker.isEnabled() ? 1 : 0,
            static_cast<unsigned>(kDefaultVolume));
}

void setVolume(uint8_t volume) { M5.Speaker.setVolume(volume); }

void playAlertPattern() {
  startSequence(kAlertPattern, sizeof(kAlertPattern) / sizeof(kAlertPattern[0]),
                "alert pattern");
}

void playChime() {
  startSequence(kChimePattern, sizeof(kChimePattern) / sizeof(kChimePattern[0]),
                "notice chime");
}

void update() {
  if (sequence == nullptr) {
    return;
  }

  if (millis() - step_started_ms < sequence[sequence_index].ms) {
    return;
  }

  sequence_index++;
  if (sequence_index >= sequence_length) {
    LOG_AUDIO("%s played", sequence_name);
    sequence = nullptr;
    sequence_length = 0;
    sequence_index = 0;
    return;
  }

  playStep();
}

bool isPlaying() { return sequence != nullptr; }

void setEnabled(bool value) {
  enabled = value;
  if (!enabled) {
    M5.Speaker.stop();
    sequence = nullptr;
    sequence_length = 0;
    sequence_index = 0;
  }
  LOG_AUDIO("sound %s", enabled ? "enabled" : "disabled");
}

bool isEnabled() { return enabled; }

}  // namespace audio
