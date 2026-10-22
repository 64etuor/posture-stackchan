#include "network.h"

#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "logging.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing. Copy include/secrets.example.h to include/secrets.h and fill in WIFI_SSID / WIFI_PASSWORD."
#endif

namespace net {
namespace {

// Give the association this long before treating the attempt as failed.
constexpr uint32_t kAttemptTimeoutMs = 15000;

// Retry backoff: 1s, 2s, 4s, ... capped at 30s (spec section 9).
constexpr uint32_t kBackoffStartMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

// One-off scan at boot. The ESP32-S3 radio is 2.4 GHz only, so the result is
// exactly the set of networks this device could ever join - which is the
// fastest way to diagnose "the SSID works on my laptop but not here".
constexpr uint32_t kScanTimeoutMs = 10000;

enum class Phase : uint8_t { SCANNING, WAITING, CONNECTING, CONNECTED };

Phase phase = Phase::WAITING;
uint32_t phase_started_ms = 0;
uint32_t backoff_ms = kBackoffStartMs;
uint32_t attempts = 0;

char ip_text[16] = {0};
char status_text[32] = {0};

// Scan bookkeeping. The STA interface needs a moment after mode() before it
// will accept a scan, so the request is issued from update(), not begin().
bool scan_requested = false;
uint8_t scan_tries = 0;

// Any timestamp below this is the 1970 default, i.e. SNTP has not replied yet.
constexpr uint64_t kClockValidAfterSec = 1700000000ULL;
bool clock_logged = false;

void startAttempt() {
  attempts++;
  phase = Phase::CONNECTING;
  phase_started_ms = millis();

  // The SSID is not a secret; the password must never reach the log.
  LOG_WIFI("Connecting to %s (attempt %lu)", WIFI_SSID,
           static_cast<unsigned long>(attempts));
  snprintf(status_text, sizeof(status_text), "WIFI ... (%lu)",
           static_cast<unsigned long>(attempts));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void scheduleRetry() {
  phase = Phase::WAITING;
  phase_started_ms = millis();
  ip_text[0] = '\0';

  snprintf(status_text, sizeof(status_text), "WIFI retry in %lus",
           static_cast<unsigned long>(backoff_ms / 1000));
  LOG_WIFI("Retry in %lu ms", static_cast<unsigned long>(backoff_ms));
}

void growBackoff() {
  backoff_ms = (backoff_ms * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff_ms * 2;
}

}  // namespace

void begin() {
  // Do not let the SDK write credentials to NVS on every boot, and drive
  // reconnection from update() so that every retry is logged and backed off.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);

  // Keeping the radio awake costs power but keeps command latency inside the
  // 1 second budget from spec section 16. The device runs on USB.
  WiFi.setSleep(false);

  backoff_ms = kBackoffStartMs;
  attempts = 0;

  phase = Phase::SCANNING;
  phase_started_ms = millis();
  scan_requested = false;
  scan_tries = 0;
  strncpy(status_text, "WIFI scan...", sizeof(status_text) - 1);
}

void update() {
  const uint32_t now = millis();

  switch (phase) {
    case Phase::SCANNING: {
      if (!scan_requested) {
        // Let the station interface settle before asking for a scan.
        if (now - phase_started_ms < 500) {
          break;
        }
        scan_tries++;
        scan_requested = true;
        phase_started_ms = now;
        LOG_WIFI("Scanning 2.4 GHz band (try %u)", static_cast<unsigned>(scan_tries));
        WiFi.scanNetworks(true /* async */, true /* include hidden */);
        break;
      }

      const int16_t found = WiFi.scanComplete();
      if (found == WIFI_SCAN_RUNNING && now - phase_started_ms < kScanTimeoutMs) {
        break;
      }

      if (found < 0 && scan_tries < 3) {
        LOG_WIFI("Scan did not complete (%d), retrying", static_cast<int>(found));
        WiFi.scanDelete();
        scan_requested = false;
        phase_started_ms = now;
        break;
      }

      if (found < 0) {
        LOG_WIFI("Scan unavailable (%d) - continuing without it",
                 static_cast<int>(found));
      } else {
        LOG_WIFI("Scan found %d network(s) reachable on 2.4 GHz:",
                 static_cast<int>(found));
        for (int16_t i = 0; i < found; i++) {
          LOG_WIFI("  \"%s\" ch=%d rssi=%d auth=%d", WiFi.SSID(i).c_str(),
                   WiFi.channel(i), WiFi.RSSI(i),
                   static_cast<int>(WiFi.encryptionType(i)));
        }
      }

      WiFi.scanDelete();
      startAttempt();
      break;
    }

    case Phase::WAITING:
      if (now - phase_started_ms >= backoff_ms) {
        startAttempt();
      }
      break;

    case Phase::CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        strncpy(ip_text, WiFi.localIP().toString().c_str(), sizeof(ip_text) - 1);
        ip_text[sizeof(ip_text) - 1] = '\0';
        strncpy(status_text, ip_text, sizeof(status_text) - 1);
        status_text[sizeof(status_text) - 1] = '\0';

        phase = Phase::CONNECTED;
        phase_started_ms = now;
        backoff_ms = kBackoffStartMs;
        attempts = 0;

        LOG_WIFI("Connected: %s (rssi %d dBm)", ip_text, WiFi.RSSI());

        // UTC only - the device never formats a local time, it just needs an
        // absolute clock to decide whether an alert arrived too late.
        configTime(0, 0, "pool.ntp.org", "time.google.com");
      } else if (now - phase_started_ms >= kAttemptTimeoutMs) {
        LOG_WIFI("Attempt timed out");
        WiFi.disconnect();
        growBackoff();
        scheduleRetry();
      }
      break;

    case Phase::CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        LOG_WIFI("Connection lost");
        WiFi.disconnect();
        // A drop is not a bad password: restart the backoff from the bottom so
        // a router reboot is picked up quickly.
        backoff_ms = kBackoffStartMs;
        scheduleRetry();
      }
      break;
  }
}

uint64_t epochMs() {
  timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) {
    return 0;
  }
  if (static_cast<uint64_t>(tv.tv_sec) < kClockValidAfterSec) {
    return 0;
  }
  if (!clock_logged) {
    clock_logged = true;
    LOG_WIFI("Clock synced (epoch %llu s)", static_cast<unsigned long long>(tv.tv_sec));
  }
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
         static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

bool isConnected() { return phase == Phase::CONNECTED; }

const char* ipAddress() { return ip_text; }

const char* statusText() { return status_text; }

}  // namespace net
