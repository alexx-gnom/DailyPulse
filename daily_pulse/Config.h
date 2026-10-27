#pragma once
#include <Arduino.h>

// DTEK data is aggregated by the existing GitHub Actions repository.
namespace Config {
constexpr bool TEST_SEND_BOTH_ON_BOOT = false; // Disable after Telegram layout test.
constexpr char DAILY_URL[] = "https://raw.githubusercontent.com/alexx-gnom/blackout-to-tg/refs/heads/main/dailypulse_calendar/daily.json";
constexpr uint32_t DAILY_CONTENT_RETRY_MS = 30000UL;
constexpr char SCHEDULE_URL[] = "https://raw.githubusercontent.com/alexx-gnom/blackout-to-tg/main/schedule.json";
constexpr char CITY[] = "м. Ірпінь";
constexpr char STREET[] = "вул. Мінеральна";
constexpr char GROUP[] = "GPV6.1";

constexpr uint32_t CHECK_EVERY_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t TELEGRAM_POLL_MS = 5000UL;
constexpr uint32_t WIFI_RETRY_MS = 15000UL;
constexpr uint32_t RETRY_REPORT_MS = 30000UL;
constexpr uint32_t RETRY_DAILY_MS = 60000UL;
constexpr int FIRST_REPORT_MINUTE = 8 * 60 + 5;
constexpr int LAST_CHECK_MINUTE = 23 * 60;
}
