#include "command_handler.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <string.h>

#include "logging.h"
#include "network.h"
#include "settings.h"

namespace commands {
namespace {

constexpr uint16_t kPort = 80;

// Remember the last few alert ids so a retransmitted alert stays silent.
constexpr size_t kSeenCapacity = 8;
constexpr size_t kIdLength = 48;

WebServer server(kPort);

Pending pending;
uint32_t last_contact_ms = 0;
bool ever_contacted = false;

char seen_ids[kSeenCapacity][kIdLength] = {};
size_t seen_next = 0;

// Values mirrored into /health by main.cpp.
AppState reported_state = AppState::BOOTING;
bool reported_muted = false;
uint32_t reported_mute_remaining_ms = 0;

void noteContact() {
  last_contact_ms = millis();
  ever_contacted = true;
}

bool seenBefore(const char* id) {
  for (size_t i = 0; i < kSeenCapacity; i++) {
    if (seen_ids[i][0] != '\0' && strncmp(seen_ids[i], id, kIdLength) == 0) {
      return true;
    }
  }
  strncpy(seen_ids[seen_next], id, kIdLength - 1);
  seen_ids[seen_next][kIdLength - 1] = '\0';
  seen_next = (seen_next + 1) % kSeenCapacity;
  return false;
}

void sendJson(int code, const char* body) {
  server.send(code, "application/json", body);
}

void sendResult(int code, const char* result, const char* detail) {
  JsonDocument doc;
  doc["result"] = result;
  if (detail != nullptr) {
    doc["detail"] = detail;
  }
  char buffer[128];
  serializeJson(doc, buffer, sizeof(buffer));
  sendJson(code, buffer);
}

// Parses the request body. Replies 400 and returns false on malformed input.
bool readBody(JsonDocument* doc) {
  const String body = server.arg("plain");
  if (body.length() == 0) {
    // An empty body is valid for endpoints where every field is optional.
    return true;
  }
  const DeserializationError error = deserializeJson(*doc, body);
  if (error) {
    LOG_HTTP("malformed JSON: %s", error.c_str());
    sendResult(400, "error", "malformed json");
    return false;
  }
  return true;
}

bool parseState(const char* name, AppState* out) {
  if (name == nullptr) return false;
  if (strcmp(name, "NORMAL") == 0) {
    *out = AppState::NORMAL;
    return true;
  }
  if (strcmp(name, "BAD_PENDING") == 0) {
    *out = AppState::BAD_PENDING;
    return true;
  }
  if (strcmp(name, "ALERT") == 0) {
    *out = AppState::ALERT;
    return true;
  }
  return false;
}

void handleHealth() {
  noteContact();

  JsonDocument doc;
  doc["device"] = "posture-stackchan";
  doc["state"] = appStateName(reported_state);
  doc["uptime_ms"] = millis();
  doc["muted"] = reported_muted;
  doc["mute_remaining_ms"] = reported_mute_remaining_ms;
  doc["sound_enabled"] = settings::get().sound_enabled;
  doc["volume"] = settings::get().volume;
  doc["brightness"] = settings::get().brightness;
  doc["ip"] = net::ipAddress();
  doc["clock_synced"] = net::epochMs() != 0;

  char buffer[256];
  serializeJson(doc, buffer, sizeof(buffer));
  sendJson(200, buffer);
}

void handleState() {
  noteContact();

  JsonDocument doc;
  if (!readBody(&doc)) return;

  AppState state;
  if (!parseState(doc["state"] | static_cast<const char*>(nullptr), &state)) {
    LOG_HTTP("/state rejected: unknown state");
    sendResult(400, "error", "state must be NORMAL, BAD_PENDING or ALERT");
    return;
  }

  pending.has_state = true;
  pending.state = state;

  const float score = doc["score"] | -1.0f;
  LOG_HTTP("/state %s (score %.2f)", appStateName(state), score);
  sendResult(200, "ok", nullptr);
}

void handleAlert() {
  noteContact();

  JsonDocument doc;
  if (!readBody(&doc)) return;

  const char* alert_id = doc["alert_id"] | static_cast<const char*>(nullptr);
  if (alert_id == nullptr || alert_id[0] == '\0') {
    LOG_HTTP("/alert rejected: missing alert_id");
    sendResult(400, "error", "alert_id required");
    return;
  }

  // Order matters and follows docs/protocol.md: a retransmission of an alert we
  // already handled is more useful to report as a duplicate than as stale.
  if (seenBefore(alert_id)) {
    LOG_HTTP("/alert %s dropped: duplicate", alert_id);
    sendResult(200, "suppressed", "duplicate");
    return;
  }

  // Stale check. Needs both a creation timestamp from the PC and a synced
  // clock here; without either we cannot tell, so we let the alert through
  // rather than silently dropping a real one.
  const uint64_t created_ms = doc["timestamp"] | 0ULL;
  const uint32_t ttl_ms = doc["expires_in_ms"] | 0UL;
  const uint64_t now_ms = net::epochMs();
  if (created_ms != 0 && ttl_ms != 0 && now_ms != 0 && now_ms > created_ms + ttl_ms) {
    LOG_HTTP("/alert %s dropped: %llu ms late", alert_id,
             static_cast<unsigned long long>(now_ms - created_ms - ttl_ms));
    sendResult(200, "suppressed", "stale");
    return;
  }

  // Answer the mute honestly rather than reporting a beep that never happened.
  if (reported_muted) {
    LOG_HTTP("/alert %s recorded but silent: muted", alert_id);
    sendResult(200, "suppressed", "muted");
    return;
  }

  pending.has_alert = true;

  const char* reason = doc["reason"] | "-";
  const float score = doc["score"] | -1.0f;
  LOG_HTTP("/alert %s accepted (reason %s, score %.2f)", alert_id, reason, score);
  sendResult(202, "fired", nullptr);
}

void handleMute() {
  noteContact();

  JsonDocument doc;
  if (!readBody(&doc)) return;

  const uint32_t requested =
      doc["duration_ms"] | settings::get().mute_duration_ms;

  pending.has_mute = true;
  pending.mute_ms = requested;

  LOG_HTTP("/mute %lums", static_cast<unsigned long>(requested));
  sendResult(200, "ok", nullptr);
}

void handleSettings() {
  noteContact();

  JsonDocument doc;
  if (!readBody(&doc)) return;

  bool changed = false;

  if (doc["volume"].is<int>()) {
    const int value = doc["volume"].as<int>();
    if (value < 0 || value > 255) {
      sendResult(400, "error", "volume must be 0-255");
      return;
    }
    settings::setVolume(static_cast<uint8_t>(value));
    changed = true;
  }

  if (doc["brightness"].is<int>()) {
    const int value = doc["brightness"].as<int>();
    if (value < 0 || value > 255) {
      sendResult(400, "error", "brightness must be 0-255");
      return;
    }
    settings::setBrightness(static_cast<uint8_t>(value));
    changed = true;
  }

  if (doc["mute_duration_ms"].is<uint32_t>()) {
    settings::setMuteDurationMs(doc["mute_duration_ms"].as<uint32_t>());
    changed = true;
  }

  if (doc["sound_enabled"].is<bool>()) {
    settings::setSoundEnabled(doc["sound_enabled"].as<bool>());
    changed = true;
  }

  if (!changed) {
    LOG_HTTP("/settings rejected: no known field");
    sendResult(400, "error", "no setting given");
    return;
  }

  const settings::Values& values = settings::get();
  JsonDocument out;
  out["result"] = "ok";
  out["volume"] = values.volume;
  out["brightness"] = values.brightness;
  out["mute_duration_ms"] = values.mute_duration_ms;
  out["sound_enabled"] = values.sound_enabled;

  char buffer[192];
  serializeJson(out, buffer, sizeof(buffer));
  sendJson(200, buffer);
}

void handleNotice() {
  noteContact();

  JsonDocument doc;
  if (!readBody(&doc)) return;

  const char* text = doc["text"] | static_cast<const char*>(nullptr);
  if (text == nullptr || text[0] == '\0') {
    LOG_HTTP("/notify rejected: missing text");
    sendResult(400, "error", "text required");
    return;
  }

  const char* kind = doc["kind"] | "info";
  if (strcmp(kind, "stretch") == 0) {
    pending.notice_kind = NoticeKind::STRETCH;
  } else if (strcmp(kind, "meal") == 0) {
    pending.notice_kind = NoticeKind::MEAL;
  } else {
    pending.notice_kind = NoticeKind::INFO;
  }

  strncpy(pending.notice_text, text, kNoticeTextSize - 1);
  pending.notice_text[kNoticeTextSize - 1] = '\0';
  pending.notice_ms = doc["duration_ms"] | 8000UL;
  pending.notice_chime = doc["chime"] | true;
  pending.has_notice = true;

  LOG_HTTP("/notify [%s] %s", kind, pending.notice_text);
  sendResult(200, "ok", nullptr);
}

void handleTest() {
  noteContact();
  pending.has_test = true;
  LOG_HTTP("/test");
  sendResult(202, "fired", nullptr);
}

void handleNotFound() {
  noteContact();
  LOG_HTTP("404 %s", server.uri().c_str());
  sendResult(404, "error", "unknown endpoint");
}

}  // namespace

void begin() {
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/state", HTTP_POST, handleState);
  server.on("/alert", HTTP_POST, handleAlert);
  server.on("/mute", HTTP_POST, handleMute);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/notify", HTTP_POST, handleNotice);
  server.on("/test", HTTP_POST, handleTest);
  server.onNotFound(handleNotFound);

  server.begin();
  LOG_HTTP("Server started on port %u", static_cast<unsigned>(kPort));
}

void update() { server.handleClient(); }

bool drain(Pending* out) {
  if (!pending.has_state && !pending.has_alert && !pending.has_mute &&
      !pending.has_test && !pending.has_notice) {
    return false;
  }
  *out = pending;
  pending = Pending{};
  return true;
}

uint32_t msSinceLastContact() {
  if (!ever_contacted) {
    return UINT32_MAX;
  }
  return millis() - last_contact_ms;
}

bool everContacted() { return ever_contacted; }

void setReportedState(AppState state, bool muted, uint32_t mute_remaining_ms) {
  reported_state = state;
  reported_muted = muted;
  reported_mute_remaining_ms = mute_remaining_ms;
}

}  // namespace commands
