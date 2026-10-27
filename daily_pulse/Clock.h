#pragma once
#include <Arduino.h>
#include <time.h>

namespace Clock {
enum class SyncStatus : uint8_t {
  NOT_STARTED, WAITING_FOR_WIFI, SYNCING, SYNCED, ERROR
};

// Ukraine EET/EEST, including automatic summer/winter time changes.
constexpr char TIMEZONE[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// Does not start or manage Wi-Fi.
void begin();
// Call regularly from loop(); does not wait for an NTP response.
void update();
bool isTimeValid();
SyncStatus status();
// Unix timestamp in seconds, or 0 when time is invalid.
time_t timestamp();
// Local Ukrainian time strings; empty if time is invalid.
String dateTime(); // YYYY-MM-DD HH:MM:SS
String date();     // YYYY-MM-DD
String time();     // HH:MM:SS
String duration(uint32_t seconds); // e.g. "2 d 03:15:08"
} // namespace Clock
