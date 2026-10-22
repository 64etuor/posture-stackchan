#pragma once

#include <stddef.h>
#include <stdint.h>

// CoreS3 built-in GC0308 camera, served to the PC as JPEG snapshots.
//
// The device still never judges posture: it only hands frames to the PC.
// Capture and JPEG encoding run in their own FreeRTOS task, so a slow encode
// never stalls the face animation, the beep sequencer or the web server.
namespace camera {

// Call once after M5.begin(). Returns false if the sensor did not start;
// the rest of the device keeps working without it.
bool begin();

bool isReady();

// Copies nothing: calls send(data, len, arg) with the latest JPEG while the
// frame is locked, so the encoder cannot free it mid-send. Returns false when
// no recent frame is available (camera down, or just woken from idle). Also
// tells the encoder someone is watching - frames are only produced while
// requests keep arriving.
using SendFn = void (*)(const uint8_t* data, size_t len, void* arg);
bool withLatestJpeg(SendFn send, void* arg);

}  // namespace camera
