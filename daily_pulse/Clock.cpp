#include "Clock.h"
#include <WiFi.h>

namespace Clock {
namespace {
constexpr time_t MIN_VALID_EPOCH = 1704067200; // 2024-01-01 UTC
constexpr uint32_t WIFI_CHECK_INTERVAL_MS = 1000;
constexpr uint32_t NTP_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t SYNC_TIMEOUT_MS = 20000;

SyncStatus currentStatus = SyncStatus::NOT_STARTED;
bool started = false;
bool ntpConfigured = false;
bool syncAttemptActive = false;
uint32_t lastWifiCheckMs = 0;
uint32_t lastNtpAttemptMs = 0;
uint32_t syncStartedMs = 0;

bool hasPlausibleTime() {
  return static_cast<time_t>(::time(nullptr)) >= MIN_VALID_EPOCH;
}

void startSyncAttempt() {
  // Starts SNTP asynchronously; it does not wait for the server.
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  ntpConfigured = true;
  syncAttemptActive = true;
  syncStartedMs = millis();
  lastNtpAttemptMs = millis();
  currentStatus = SyncStatus::SYNCING;
}
} // namespace

void begin() {
  started = true;
  ntpConfigured = false;
  syncAttemptActive = false;
  lastWifiCheckMs = 0;
  lastNtpAttemptMs = 0;

  setenv("TZ", TIMEZONE, 1);
  tzset();

  if (hasPlausibleTime()) {
    currentStatus = SyncStatus::SYNCED;
  } else if (WiFi.status() == WL_CONNECTED) {
    startSyncAttempt();
  } else {
    currentStatus = SyncStatus::WAITING_FOR_WIFI;
  }
}

void update() {
  if (!started) return;

  const uint32_t nowMs = millis();

  if (hasPlausibleTime()) {
    currentStatus = SyncStatus::SYNCED;
    syncAttemptActive = false;
    return;
  }

  if (nowMs - lastWifiCheckMs < WIFI_CHECK_INTERVAL_MS) {
    if (syncAttemptActive && nowMs - syncStartedMs >= SYNC_TIMEOUT_MS) {
      syncAttemptActive = false;
      currentStatus = SyncStatus::ERROR;
    }
    return;
  }
  lastWifiCheckMs = nowMs;

  if (WiFi.status() != WL_CONNECTED) {
    syncAttemptActive = false;
    currentStatus = SyncStatus::WAITING_FOR_WIFI;
    return;
  }

  if (!ntpConfigured ||
      (!syncAttemptActive &&
       (currentStatus == SyncStatus::ERROR ||
        nowMs - lastNtpAttemptMs >= NTP_RETRY_INTERVAL_MS))) {
    startSyncAttempt();
    return;
  }

  if (syncAttemptActive && nowMs - syncStartedMs >= SYNC_TIMEOUT_MS) {
    syncAttemptActive = false;
    currentStatus = SyncStatus::ERROR;
  }
}

bool isTimeValid() { return started && hasPlausibleTime(); }
SyncStatus status() { return currentStatus; }

time_t timestamp() {
  return isTimeValid() ? ::time(nullptr) : 0;
}

String dateTime() {
  if (!isTimeValid()) return "";
  time_t now = ::time(nullptr);
  struct tm localInfo;
  if (localtime_r(&now, &localInfo) == nullptr) return "";
  char buffer[20];
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localInfo) == 0) return "";
  return String(buffer);
}

String date() {
  if (!isTimeValid()) return "";
  time_t now = ::time(nullptr);
  struct tm localInfo;
  if (localtime_r(&now, &localInfo) == nullptr) return "";
  char buffer[11];
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d", &localInfo) == 0) return "";
  return String(buffer);
}

String time() {
  if (!isTimeValid()) return "";
  time_t now = ::time(nullptr);
  struct tm localInfo;
  if (localtime_r(&now, &localInfo) == nullptr) return "";
  char buffer[9];
  if (strftime(buffer, sizeof(buffer), "%H:%M:%S", &localInfo) == 0) return "";
  return String(buffer);
}

String duration(uint32_t seconds) {
  const uint32_t days = seconds / 86400UL;
  const uint32_t hours = (seconds % 86400UL) / 3600UL;
  const uint32_t minutes = (seconds % 3600UL) / 60UL;
  const uint32_t secs = seconds % 60UL;
  char buffer[32];

  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%lu d %02lu:%02lu:%02lu",
      static_cast<unsigned long>(days),
      static_cast<unsigned long>(hours),
      static_cast<unsigned long>(minutes),
      static_cast<unsigned long>(secs));
  } else {
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu",
      static_cast<unsigned long>(hours),
      static_cast<unsigned long>(minutes),
      static_cast<unsigned long>(secs));
  }
  return String(buffer);
}
} // namespace Clock
