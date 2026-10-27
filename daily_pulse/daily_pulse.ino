#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "Clock.h"
#include "Config.h"
#include "ScheduleService.h"
#include "TelegramService.h"
#include "DailyContentService.h"

Preferences prefs;
String lastSchedule = "";
String lastReportDate = "";
String lastFactUpdate = "";
String tgOffset = "0";
uint32_t lastCheckMs = 0;
uint32_t lastTelegramMs = 0;
uint32_t lastWifiMs = 0;
uint32_t lastBootReportAttemptMs = 0;
uint32_t lastDailyAttemptMs = 0;
bool bootReportSent = false;
// Test-only state: intentionally resets at each boot.
bool previewMorningSent = false;
bool previewEveningSent = false;
uint32_t lastPreviewAttemptMs = 0;

void sendStartupPreview() {
  if (!Config::TEST_SEND_BOTH_ON_BOOT || previewEveningSent) return;
  const uint32_t now = millis();
  if (lastPreviewAttemptMs != 0 && now - lastPreviewAttemptMs < Config::DAILY_CONTENT_RETRY_MS) return;
  lastPreviewAttemptMs = now;
  DailyContent content;
  String error;
  if (!fetchDailyContent(Clock::date(), content, error)) {
    Serial.printf("[DAILY] Fetch failed: %s\n", error.c_str());
    return;
  }
  if (!previewMorningSent) {
    Serial.println("[DAILY] Sending morning preview...");
    // Even if the image is unavailable, show the text to test its layout.
    const bool photoOk = sendTelegramPhoto(content.morningImageUrl, content.morningCaption);
    if (!photoOk) {
      Serial.println("[DAILY] Morning photo failed. Will retry on next attempt.");
      return;
    }
    previewMorningSent = true;
    if (content.morningExtra.length()) {
      // This may be sent again if the next step fails; acceptable for preview mode.
      if (!sendTelegram(content.morningExtra)) {
        Serial.println("[DAILY] Extra holiday text failed.");
      }
    }
    delay(1000);
  }
  if (!previewEveningSent) {
    Serial.println("[DAILY] Sending evening preview...");
    if (!sendTelegramPhoto(content.eveningImageUrl, content.eveningCaption)) {
      Serial.println("[DAILY] Evening photo failed. Will retry evening only.");
      return;
    }
    previewEveningSent = true;
    Serial.println("[DAILY] Both preview photos delivered.");
  }
}


void checkTelegramCommands() {
  if (millis() - lastTelegramMs < Config::TELEGRAM_POLL_MS) return;
  lastTelegramMs = millis();
  if (WiFi.status() != WL_CONNECTED || String(TELEGRAM_BOT_TOKEN).startsWith("PUT_")) return;

  JsonDocument doc;
  if (!getTelegramUpdates(doc, TELEGRAM_BOT_TOKEN, tgOffset)) return;
  for (JsonObject update : doc["result"].as<JsonArray>()) {
    long id = update["update_id"] | 0L;
    if (id >= tgOffset.toInt()) {
      tgOffset = String(id + 1);
      prefs.putString("tgOffset", tgOffset);
    }
    String chat = update["message"]["chat"]["id"].as<String>();
    String text = update["message"]["text"] | "";
    text.trim();
    if (chat != String(TELEGRAM_CHAT_ID)) continue;

    if (text == "/schedule" || text.startsWith("/schedule@")) {
      String schedule, updateTime, error;
      if (fetchSchedule(schedule, updateTime, error)) {
        sendTelegram(makeReportMessage(schedule, updateTime));
      } else {
        sendTelegram("⚠️ Не вдалося отримати свіжий графік ДТЕК: " + error + ". Попередній графік не вважається актуальним.");
      }
    } else if (text == "/start" || text == "/help") {
      sendTelegram("Бот графіків ДТЕК, Ірпінь, черга 6.1.\n/schedule — отримати графік зараз.");
    }
  }
}

bool checkSchedule(bool startupReport = false) {
  const String today = Clock::date();
  if (!today.length()) {
    Serial.println("[TIME] Час не синхронізовано; графік не запитую.");
    return false;
  }

  String schedule, update, error;
  if (!fetchSchedule(schedule, update, error)) {
    Serial.printf("[DTEK] ERROR: %s\n", error.c_str());
    return false;
  }

  const int minute = minuteOfDay();
  const bool dailyDue = lastReportDate != today &&
                        minute >= Config::FIRST_REPORT_MINUTE &&
                        minute <= Config::LAST_CHECK_MINUTE;
  const bool changed = lastSchedule.length() &&
                       (schedule != lastSchedule || update != lastFactUpdate);
  bool reportOk = true;

  if (startupReport || dailyDue) {
    reportOk = sendTelegram(makeReportMessage(schedule, update));
    if (reportOk && minute >= Config::FIRST_REPORT_MINUTE && minute <= Config::LAST_CHECK_MINUTE) {
      lastReportDate = today;
      prefs.putString("reportDate", lastReportDate);
    }
  } else if (changed) {
    reportOk = sendTelegram(makeReportMessage(schedule, update, true));
  }

  if (changed && !reportOk) return false;
  lastSchedule = schedule;
  lastFactUpdate = update;
  prefs.putString("schedule", lastSchedule);
  prefs.putString("factUpdate", lastFactUpdate);
  lastCheckMs = millis();
  return reportOk;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== DailyPulse START ===");

  prefs.begin("blackoutbot", false);
  lastSchedule = prefs.getString("schedule", "");
  lastReportDate = prefs.getString("reportDate", "");
  lastFactUpdate = prefs.getString("factUpdate", "");
  tgOffset = prefs.getString("tgOffset", "0");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Clock::begin();
  Serial.println("[BOOT] DailyPulse стартував. Чекаю Wi-Fi та синхронізацію часу.");
}

void loop() {
  static uint32_t lastLog = 0;
  const uint32_t nowMs = millis();
  Clock::update();

  if (nowMs - lastLog >= 5000) {
    lastLog = nowMs;
    Serial.printf("[ALIVE] %s | Wi-Fi=%s | Clock=%s\n",
      Clock::dateTime().c_str(),
      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
      Clock::isTimeValid() ? "valid" : "waiting");
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (nowMs - lastWifiMs >= Config::WIFI_RETRY_MS) {
      lastWifiMs = nowMs;
      Serial.println("[WiFi] Перепідключення...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(10);
    return;
  }

  if (!Clock::isTimeValid()) {
    delay(10);
    return;
  }

  checkTelegramCommands();
  sendStartupPreview();

  if (!bootReportSent &&
      (lastBootReportAttemptMs == 0 || nowMs - lastBootReportAttemptMs >= Config::RETRY_REPORT_MS)) {
    lastBootReportAttemptMs = nowMs;
    Serial.println("[BOOT] Спроба надіслати стартовий звіт...");
    if (checkSchedule(true)) {
      bootReportSent = true;
      Serial.println("[BOOT] Стартовий звіт надіслано.");
    } else {
      Serial.println("[BOOT] Стартовий звіт не надіслано; повторю спробу пізніше.");
    }
  }

  const int minute = minuteOfDay();
  if (minute >= Config::FIRST_REPORT_MINUTE && minute <= Config::LAST_CHECK_MINUTE) {
    const String today = Clock::date();
    if (lastReportDate != today &&
        (lastDailyAttemptMs == 0 || nowMs - lastDailyAttemptMs >= Config::RETRY_DAILY_MS)) {
      lastDailyAttemptMs = nowMs;
      checkSchedule(false);
    }
    if (lastCheckMs == 0 || nowMs - lastCheckMs >= Config::CHECK_EVERY_MS) {
      checkSchedule(false);
    }
  }
  delay(10);
}
