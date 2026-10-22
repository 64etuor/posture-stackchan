#pragma once

#include <stdint.h>

// Wi-Fi station management. Fully non-blocking: begin() kicks off the first
// attempt and update() drives connect / timeout / backoff / reconnect without
// ever waiting on the radio.
namespace net {

void begin();

// Call once per loop().
void update();

bool isConnected();

// "192.168.0.31" while connected, "" otherwise.
const char* ipAddress();

// Short line for the display: the IP when up, otherwise what we are waiting on.
const char* statusText();

// Wall-clock time in milliseconds since the Unix epoch, or 0 while the clock
// has not been set by SNTP yet. Needed to judge whether an alert is stale.
uint64_t epochMs();

}  // namespace net
