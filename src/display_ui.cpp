#include "display_ui.h"

#include <M5Unified.h>

#include "logging.h"
#include "settings.h"

namespace ui {
namespace {

// CoreS3 display is 320x240.
constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kBarHeight = 34;

constexpr int kEyeLeftX = 104;
constexpr int kEyeRightX = 216;
constexpr int kEyeY = 128;
constexpr int kEyeRadius = 30;
constexpr int kBrowOffsetY = 46;
constexpr int kMouthY = 190;

constexpr uint16_t kBgColor = 0x0000;      // black, StackChan style
constexpr uint16_t kFaceColor = 0xFFFF;    // white eyes / mouth
constexpr uint16_t kBarTextColor = 0x0000;

// Blink timing (milliseconds).
constexpr uint32_t kBlinkClosedMs = 120;
constexpr uint32_t kBlinkMinIntervalMs = 2500;
constexpr uint32_t kBlinkMaxIntervalMs = 6000;

enum class EyeKind : uint8_t { ROUND, SLEEPY, CROSS };
enum class BrowKind : uint8_t { NONE, WORRIED, ANGRY };
enum class MouthKind : uint8_t { SMILE, WAVY, OPEN, FLAT };

struct FaceStyle {
  uint16_t bar_color;
  const char* label;
  EyeKind eye;
  BrowKind brow;
  MouthKind mouth;
  bool blinks;
};

FaceStyle styleFor(AppState state) {
  switch (state) {
    case AppState::BOOTING:
      return {0x7BEF, "STARTING", EyeKind::SLEEPY, BrowKind::NONE, MouthKind::FLAT, false};
    case AppState::CONNECTING:
      return {0x07FF, "CONNECTING", EyeKind::ROUND, BrowKind::NONE, MouthKind::FLAT, true};
    case AppState::NORMAL:
      return {0x07E0, "POSTURE OK", EyeKind::ROUND, BrowKind::NONE, MouthKind::SMILE, true};
    case AppState::BAD_PENDING:
      return {0xFFE0, "CHECK POSTURE", EyeKind::ROUND, BrowKind::WORRIED, MouthKind::WAVY, true};
    case AppState::ALERT:
      return {0xF800, "STRAIGHTEN UP", EyeKind::ROUND, BrowKind::ANGRY, MouthKind::OPEN, false};
    case AppState::MUTED:
      return {0x7BEF, "MUTED", EyeKind::SLEEPY, BrowKind::NONE, MouthKind::SMILE, false};
    case AppState::OFFLINE:
      return {0x7BEF, "PC OFFLINE", EyeKind::CROSS, BrowKind::NONE, MouthKind::WAVY, false};
    case AppState::FAULT:
      return {0xF800, "ERROR", EyeKind::CROSS, BrowKind::ANGRY, MouthKind::FLAT, false};
    case AppState::NOTICE:
      // Drawn by renderNotice(); this entry only keeps the blink logic quiet.
      return {0x07FF, "NOTICE", EyeKind::ROUND, BrowKind::NONE, MouthKind::SMILE, false};
  }
  return {0x7BEF, "UNKNOWN", EyeKind::ROUND, BrowKind::NONE, MouthKind::FLAT, false};
}

// Settings screen layout. Four full-width rows with a large [-] and [+] pad on
// each side, and a close bar along the bottom - no small targets (spec 7).
constexpr int kSettingsTop = 38;
constexpr int kRowHeight = 40;
constexpr int kRowPitch = 42;
constexpr int kRowCount = 4;
constexpr int kPadWidth = 76;
constexpr int kCloseTop = 208;

// Top-right corner of the face screen opens the settings.
constexpr int kCornerWidth = 70;

M5Canvas canvas(&M5.Display);

Screen current_screen = Screen::FACE;
AppState current_state = AppState::BOOTING;

char notice_text[160] = {0};
char notice_label[16] = {0};
uint16_t notice_color = 0x07FF;
char sub_text[32] = {0};

bool eyes_closed = false;
uint32_t next_blink_at_ms = 0;
uint32_t blink_started_ms = 0;
bool dirty = true;

uint32_t nextBlinkDelay() {
  return kBlinkMinIntervalMs + static_cast<uint32_t>(random(kBlinkMaxIntervalMs - kBlinkMinIntervalMs));
}

void drawEye(int cx, bool closed, EyeKind kind) {
  if (kind == EyeKind::CROSS) {
    const int r = kEyeRadius - 4;
    canvas.drawWideLine(cx - r, kEyeY - r, cx + r, kEyeY + r, 7, kFaceColor);
    canvas.drawWideLine(cx - r, kEyeY + r, cx + r, kEyeY - r, 7, kFaceColor);
    return;
  }

  if (kind == EyeKind::SLEEPY || closed) {
    canvas.fillRoundRect(cx - kEyeRadius, kEyeY - 4, kEyeRadius * 2, 8, 4, kFaceColor);
    return;
  }

  canvas.fillCircle(cx, kEyeY, kEyeRadius, kFaceColor);
}

void drawBrow(int cx, BrowKind kind, bool mirrored) {
  if (kind == BrowKind::NONE) {
    return;
  }

  const int y = kEyeY - kBrowOffsetY;
  const int half = kEyeRadius;
  // ANGRY: inner end low, outer end high. WORRIED: the opposite.
  const int inner_dy = (kind == BrowKind::ANGRY) ? 12 : -12;
  const int inner_x = mirrored ? cx - half : cx + half;
  const int outer_x = mirrored ? cx + half : cx - half;

  canvas.drawWideLine(outer_x, y - inner_dy, inner_x, y + inner_dy, 8, kFaceColor);
}

void drawMouth(MouthKind kind) {
  switch (kind) {
    case MouthKind::SMILE:
      // Lower half of a ring: 0 deg is 3 o'clock, angles increase clockwise.
      canvas.fillArc(kWidth / 2, kMouthY - 18, 26, 33, 20, 160, kFaceColor);
      break;
    case MouthKind::WAVY:
      canvas.fillArc(kWidth / 2, kMouthY + 18, 26, 33, 200, 340, kFaceColor);
      break;
    case MouthKind::OPEN:
      canvas.fillRoundRect(kWidth / 2 - 28, kMouthY - 16, 56, 32, 10, kFaceColor);
      break;
    case MouthKind::FLAT:
      canvas.fillRoundRect(kWidth / 2 - 30, kMouthY - 4, 60, 8, 4, kFaceColor);
      break;
  }
}

void render() {
  const FaceStyle style = styleFor(current_state);

  canvas.fillSprite(kBgColor);

  // Status bar
  canvas.fillRect(0, 0, kWidth, kBarHeight, style.bar_color);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(kBarTextColor);
  canvas.setFont(&fonts::Font4);
  canvas.drawString(style.label, kWidth / 2, kBarHeight / 2);

  // Face
  drawBrow(kEyeLeftX, style.brow, false);
  drawBrow(kEyeRightX, style.brow, true);
  drawEye(kEyeLeftX, eyes_closed, style.eye);
  drawEye(kEyeRightX, eyes_closed, style.eye);
  drawMouth(style.mouth);

  // Secondary line
  if (sub_text[0] != '\0') {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(0xC618);
    canvas.drawString(sub_text, kWidth / 2, kHeight - 14);
  }

  // Hint that the top-right corner opens the settings.
  canvas.fillCircle(kWidth - 40, kBarHeight / 2, 3, kBarTextColor);
  canvas.fillCircle(kWidth - 28, kBarHeight / 2, 3, kBarTextColor);
  canvas.fillCircle(kWidth - 16, kBarHeight / 2, 3, kBarTextColor);

  canvas.pushSprite(0, 0);
}

void drawSettingsRow(int index, const char* label, const char* value,
                     const char* minus, const char* plus) {
  const int top = kSettingsTop + index * kRowPitch;
  const int middle = top + kRowHeight / 2;

  canvas.fillRoundRect(0, top, kPadWidth, kRowHeight, 6, 0x39C7);
  canvas.fillRoundRect(kWidth - kPadWidth, top, kPadWidth, kRowHeight, 6, 0x39C7);

  canvas.setTextDatum(middle_center);
  canvas.setTextColor(kFaceColor);
  canvas.setFont(&fonts::Font4);
  canvas.drawString(minus, kPadWidth / 2, middle);
  canvas.drawString(plus, kWidth - kPadWidth / 2, middle);
  canvas.drawString(value, kWidth / 2, middle + 6);

  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(0x8410);
  canvas.drawString(label, kWidth / 2, top + 10);
}

size_t utf8CharLen(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;  // malformed input: consume a byte so we always make progress
}

// Copies whole UTF-8 characters from `text` into `line` while they fit inside
// max_width, and returns how many bytes of `text` were consumed. Korean text
// must never be split mid-character, which is why this cannot be a byte count.
size_t takeLine(const char* text, int max_width, char* line, size_t line_size) {
  size_t consumed = 0;
  size_t used = 0;
  line[0] = '\0';

  while (text[consumed] != '\0') {
    if (text[consumed] == '\n') {
      consumed++;
      break;
    }

    const size_t char_len = utf8CharLen(static_cast<unsigned char>(text[consumed]));
    if (used + char_len + 1 > line_size) {
      break;
    }

    memcpy(line + used, text + consumed, char_len);
    line[used + char_len] = '\0';

    if (canvas.textWidth(line) > max_width) {
      line[used] = '\0';  // undo: this character does not fit
      break;
    }

    used += char_len;
    consumed += char_len;
  }

  return consumed;
}

void renderNotice() {
  canvas.fillSprite(kBgColor);

  canvas.fillRect(0, 0, kWidth, kBarHeight, notice_color);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(kBarTextColor);
  canvas.setFont(&fonts::Font4);
  canvas.drawString(notice_label, kWidth / 2, kBarHeight / 2);

  // A small friendly face above the message.
  canvas.fillCircle(kWidth / 2 - 46, 74, 12, kFaceColor);
  canvas.fillCircle(kWidth / 2 + 46, 74, 12, kFaceColor);
  canvas.fillArc(kWidth / 2, 82, 18, 24, 20, 160, kFaceColor);

  canvas.setFont(&fonts::efontKR_16);
  canvas.setTextColor(kFaceColor);

  const int max_width = kWidth - 24;
  const char* cursor = notice_text;
  char line[80];
  int y = 142;

  for (int i = 0; i < 4 && *cursor != '\0'; i++) {
    size_t consumed = takeLine(cursor, max_width, line, sizeof(line));
    if (consumed == 0) {
      // One character wider than the screen: emit it anyway so we advance.
      consumed = utf8CharLen(static_cast<unsigned char>(*cursor));
      memcpy(line, cursor, consumed);
      line[consumed] = '\0';
    }
    canvas.drawString(line, kWidth / 2, y);
    cursor += consumed;
    y += 24;
  }

  canvas.pushSprite(0, 0);
}

void renderSettings() {
  const settings::Values& v = settings::get();

  canvas.fillSprite(kBgColor);

  canvas.fillRect(0, 0, kWidth, kBarHeight, 0x07FF);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(kBarTextColor);
  canvas.setFont(&fonts::Font4);
  canvas.drawString("SETTINGS", kWidth / 2, kBarHeight / 2);

  char value[16];

  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(v.volume));
  drawSettingsRow(0, "VOLUME", value, "-", "+");

  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(v.brightness));
  drawSettingsRow(1, "BRIGHTNESS", value, "-", "+");

  snprintf(value, sizeof(value), "%lu min",
           static_cast<unsigned long>(v.mute_duration_ms / 60000UL));
  drawSettingsRow(2, "MUTE TIME", value, "-", "+");

  drawSettingsRow(3, "ALERT SOUND", v.sound_enabled ? "ON" : "OFF", "OFF", "ON");

  canvas.fillRoundRect(0, kCloseTop, kWidth, kHeight - kCloseTop, 8, 0x07E0);
  canvas.setTextColor(kBarTextColor);
  canvas.setFont(&fonts::Font4);
  canvas.drawString("CLOSE", kWidth / 2, kCloseTop + (kHeight - kCloseTop) / 2);

  canvas.pushSprite(0, 0);
}

}  // namespace

void begin() {
  M5.Display.setRotation(1);
  M5.Display.setBrightness(140);
  M5.Display.fillScreen(kBgColor);

  // 320x240 at 16bpp is 150 KB, which belongs in PSRAM. If PSRAM is missing or
  // failed to initialise, fall back to internal RAM so the device still boots
  // with a face instead of a black screen.
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(kWidth, kHeight)) {
    LOG_DISPLAY("PSRAM sprite failed (psram=%u bytes), retrying in internal RAM",
                static_cast<unsigned>(ESP.getPsramSize()));
    canvas.setPsram(false);
    if (!canvas.createSprite(kWidth, kHeight)) {
      LOG_DISPLAY("sprite allocation failed - display disabled");
    }
  }

  randomSeed(esp_random());
  next_blink_at_ms = millis() + nextBlinkDelay();
  dirty = true;

  LOG_DISPLAY("UI ready (%dx%d)", kWidth, kHeight);
}

void setState(AppState state) {
  if (state == current_state) {
    return;
  }
  current_state = state;
  // A state change interrupts a blink so the new expression is fully visible.
  eyes_closed = false;
  next_blink_at_ms = millis() + nextBlinkDelay();
  dirty = true;
}

void setSubText(const char* text) {
  const char* value = (text != nullptr) ? text : "";
  if (strncmp(sub_text, value, sizeof(sub_text)) == 0) {
    return;
  }
  strncpy(sub_text, value, sizeof(sub_text) - 1);
  sub_text[sizeof(sub_text) - 1] = '\0';
  dirty = true;
}

void markDirty() { dirty = true; }

void setNotice(const char* text, const char* label, uint16_t bar_color) {
  strncpy(notice_text, text ? text : "", sizeof(notice_text) - 1);
  notice_text[sizeof(notice_text) - 1] = '\0';
  strncpy(notice_label, label ? label : "NOTICE", sizeof(notice_label) - 1);
  notice_label[sizeof(notice_label) - 1] = '\0';
  notice_color = bar_color;
  dirty = true;
}

void setScreen(Screen value) {
  if (value == current_screen) {
    return;
  }
  current_screen = value;
  dirty = true;
  LOG_DISPLAY("screen -> %s", value == Screen::SETTINGS ? "SETTINGS" : "FACE");
}

Screen screen() { return current_screen; }

bool hitTestSettingsCorner(int16_t x, int16_t y) {
  return y < kBarHeight && x > kWidth - kCornerWidth;
}

SettingsAction hitTestSettings(int16_t x, int16_t y) {
  if (y >= kCloseTop) {
    return SettingsAction::CLOSE;
  }
  if (y < kSettingsTop) {
    return SettingsAction::NONE;
  }

  const int row = (y - kSettingsTop) / kRowPitch;
  if (row < 0 || row >= kRowCount) {
    return SettingsAction::NONE;
  }

  const bool minus = x < kPadWidth;
  const bool plus = x > kWidth - kPadWidth;
  if (!minus && !plus) {
    return SettingsAction::NONE;
  }

  switch (row) {
    case 0:
      return minus ? SettingsAction::VOLUME_DOWN : SettingsAction::VOLUME_UP;
    case 1:
      return minus ? SettingsAction::BRIGHTNESS_DOWN : SettingsAction::BRIGHTNESS_UP;
    case 2:
      return minus ? SettingsAction::MUTE_DOWN : SettingsAction::MUTE_UP;
    default:
      return minus ? SettingsAction::SOUND_OFF : SettingsAction::SOUND_ON;
  }
}

void update() {
  if (current_screen == Screen::SETTINGS) {
    if (dirty) {
      renderSettings();
      dirty = false;
    }
    return;
  }

  if (current_state == AppState::NOTICE) {
    if (dirty) {
      renderNotice();
      dirty = false;
    }
    return;
  }

  const uint32_t now = millis();
  const FaceStyle style = styleFor(current_state);

  if (!style.blinks) {
    if (eyes_closed) {
      eyes_closed = false;
      dirty = true;
    }
  } else if (eyes_closed) {
    if (now - blink_started_ms >= kBlinkClosedMs) {
      eyes_closed = false;
      next_blink_at_ms = now + nextBlinkDelay();
      dirty = true;
    }
  } else if (static_cast<int32_t>(now - next_blink_at_ms) >= 0) {
    eyes_closed = true;
    blink_started_ms = now;
    dirty = true;
  }

  if (dirty) {
    render();
    dirty = false;
  }
}

}  // namespace ui
