#pragma once

#include <Arduino.h>

// Single logging entry point so every module produces the same
// "[TAG] message" format required by the project spec (section 11).
//
// Never log secrets (Wi-Fi password, tokens) through these macros.

#define LOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\r\n", ##__VA_ARGS__)

#define LOG_BOOT(fmt, ...) LOG("BOOT", fmt, ##__VA_ARGS__)
#define LOG_DISPLAY(fmt, ...) LOG("DISPLAY", fmt, ##__VA_ARGS__)
#define LOG_AUDIO(fmt, ...) LOG("AUDIO", fmt, ##__VA_ARGS__)
#define LOG_TOUCH(fmt, ...) LOG("TOUCH", fmt, ##__VA_ARGS__)
#define LOG_WIFI(fmt, ...) LOG("WIFI", fmt, ##__VA_ARGS__)
#define LOG_HTTP(fmt, ...) LOG("HTTP", fmt, ##__VA_ARGS__)
#define LOG_SETTINGS(fmt, ...) LOG("SETTINGS", fmt, ##__VA_ARGS__)
#define LOG_CAM(fmt, ...) LOG("CAM", fmt, ##__VA_ARGS__)
#define LOG_STATE(fmt, ...) LOG("STATE", fmt, ##__VA_ARGS__)
