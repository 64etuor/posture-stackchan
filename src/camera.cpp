#include "camera.h"

#include <M5Unified.h>
#include <esp_camera.h>
#include <esp_log.h>
#include <img_converters.h>

#include "logging.h"

namespace camera {
namespace {

// Encode only while the PC is pulling frames; idle otherwise.
constexpr uint32_t kIdleAfterMs = 3000;
// Upper bound on the frame rate the encoder will produce (10 fps).
constexpr uint32_t kMinFrameIntervalMs = 100;
// Sleep at least this long after every frame, however slow the encode was.
// Without it a >100 ms encode loops back with no pause, starves IDLE0 on
// core 0 and the task watchdog reboots the device after 5 s.
constexpr uint32_t kMinYieldMs = 20;
constexpr uint8_t kJpegQuality = 80;
// A frame older than this is not served: after an idle period the buffer still
// holds whatever was captured before the PC went away.
constexpr uint32_t kMaxFrameAgeMs = 1000;

bool ready = false;

SemaphoreHandle_t frame_lock = nullptr;
uint8_t* latest_jpeg = nullptr;  // owned; allocated by frame2jpg()
size_t latest_len = 0;
uint32_t latest_at_ms = 0;

volatile uint32_t last_request_ms = 0;
volatile bool ever_requested = false;

void encoderTask(void*) {
  uint32_t frames = 0;
  uint32_t window_start = millis();
  bool grab_failing = false;

  for (;;) {
    const bool wanted =
        ever_requested && millis() - last_request_ms < kIdleAfterMs;
    if (!wanted) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const uint32_t started = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      if (!grab_failing) {
        LOG_CAM("frame grab failed");
        grab_failing = true;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (grab_failing) {
      LOG_CAM("frame grab recovered");
      grab_failing = false;
    }

    uint8_t* jpeg = nullptr;
    size_t len = 0;
    const bool ok = frame2jpg(fb, kJpegQuality, &jpeg, &len);
    esp_camera_fb_return(fb);

    if (!ok) {
      LOG_CAM("jpeg encode failed");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    xSemaphoreTake(frame_lock, portMAX_DELAY);
    uint8_t* old = latest_jpeg;
    latest_jpeg = jpeg;
    latest_len = len;
    latest_at_ms = millis();
    xSemaphoreGive(frame_lock);
    free(old);

    frames++;
    if (millis() - window_start >= 30000) {
      LOG_CAM("streaming %.1f fps, last frame %u bytes",
              frames * 1000.0f / (millis() - window_start),
              static_cast<unsigned>(len));
      frames = 0;
      window_start = millis();
    }

    const uint32_t spent = millis() - started;
    const uint32_t rest = spent + kMinYieldMs < kMinFrameIntervalMs
                              ? kMinFrameIntervalMs - spent
                              : kMinYieldMs;
    vTaskDelay(pdMS_TO_TICKS(rest));
  }
}

}  // namespace

bool begin() {
  // Pin map from M5Stack's official M5CoreS3 library (src/utility/GC0308.cpp).
  // The camera's SCCB shares the internal I2C bus (SDA 12 / SCL 11) with touch
  // and power management. Releasing M5Unified's hold lets the camera driver
  // install on that port; M5GFX re-applies its own settings per transaction,
  // so touch keeps working afterwards. Same sequence as GC0308::begin().
  camera_config_t config = {};
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.pin_xclk = -1;
  config.pin_sccb_sda = 12;
  config.pin_sccb_scl = 11;
  config.pin_d7 = 47;
  config.pin_d6 = 48;
  config.pin_d5 = 16;
  config.pin_d4 = 15;
  config.pin_d3 = 42;
  config.pin_d2 = 41;
  config.pin_d1 = 40;
  config.pin_d0 = 39;
  config.pin_vsync = 46;
  config.pin_href = 38;
  config.pin_pclk = 45;
  // Keep 20 MHz as in the official config. 16 MHz would switch this
  // esp32-camera build to direct-to-PSRAM DMA, which produced torn, colour-
  // banded frames on hardware.
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  // GC0308 has no hardware JPEG; frames are encoded in software.
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 0;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  // Always hand out the newest frame, not one that sat in the queue.
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.sccb_i2c_port = -1;

  // The driver's cam_task has a 2 KB stack; a single "FB-SIZE" error log
  // (an occasional short frame, which it already drops) overflows it and the
  // device boot-loops on a stack canary panic. Our own [CAM] lines report
  // failures instead.
  esp_log_level_set("cam_hal", ESP_LOG_NONE);

  M5.In_I2C.release();
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOG_CAM("init failed: 0x%x", static_cast<unsigned>(err));
    return false;
  }

  frame_lock = xSemaphoreCreateMutex();
  // Core 0 alongside Wi-Fi; loop() runs on core 1 and keeps the face smooth.
  xTaskCreatePinnedToCore(encoderTask, "cam", 8192, nullptr, 1, nullptr, 0);

  ready = true;
  LOG_CAM("GC0308 ready (320x240, jpeg q%u)", static_cast<unsigned>(kJpegQuality));
  return true;
}

bool isReady() { return ready; }

bool withLatestJpeg(SendFn send, void* arg) {
  last_request_ms = millis();
  ever_requested = true;

  if (!ready) {
    return false;
  }

  xSemaphoreTake(frame_lock, portMAX_DELAY);
  const bool have = latest_jpeg != nullptr &&
                    millis() - latest_at_ms < kMaxFrameAgeMs;
  if (have) {
    send(latest_jpeg, latest_len, arg);
  }
  xSemaphoreGive(frame_lock);
  return have;
}

}  // namespace camera
