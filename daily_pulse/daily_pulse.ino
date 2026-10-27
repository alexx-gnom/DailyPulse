#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include "secrets.h"
#include "Clock.h"

// Irpin, queue 6.1. ArduinoJson 7.x required.
static const char* DTEK_BASE = "https://www.dtek-krem.com.ua";
static const char* DTEK_AJAX = "https://www.dtek-krem.com.ua/ua/ajax";
static const char* DTEK_REFERER = "https://www.dtek-krem.com.ua/ua/shutdowns";
static const char* CITY = "м. Ірпінь";
static const char* STREET = "вул. Мінеральна";
static const char* GROUP = "GPV6.1";

static const uint32_t CHECK_EVERY_MS = 5UL * 60UL * 1000UL;
static const uint32_t TELEGRAM_POLL_MS = 5000;
static const uint32_t WIFI_RETRY_MS = 15000;
static const int FIRST_REPORT_MINUTE = 8 * 60 + 5;
static const int LAST_CHECK_MINUTE = 23 * 60;

Preferences prefs;
String lastSchedule = "";
String lastReportDate = "";
String lastFactUpdate = "";
String tgOffset = "0";
uint32_t lastCheckMs = 0, lastTelegramMs = 0, lastWifiMs = 0, lastBootReportAttemptMs = 0, lastDailyAttemptMs = 0;
int lastCheckedMinute = -1;
bool bootReportSent = false;

String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out; out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
String jsonEscape(const String& s) {
  String out; out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}
String nowDate() {
  return Clock::date();
}

String nowDateUa() {
  if (!Clock::isTimeValid()) return "дата невідома";

  time_t now = Clock::timestamp();
  struct tm t;
  if (!localtime_r(&now, &t)) return "дата невідома";

  const char* days[] = {
    "неділю", "понеділок", "вівторок", "середу",
    "четвер", "п’ятницю", "суботу"
  };

  char b[20];
  strftime(b, sizeof(b), "%d.%m.%Y", &t);

  return String(days[t.tm_wday]) + ", " + b;
}


String makeReportMessage(const String& schedule,
                         const String& update,
                         bool changed = false) {
  String msg = changed
    ? "🔔 <b>ГРАФІК ЗМІНИВСЯ</b>\n\n"
    : "🌇 <b>ГРАФІК ВІДКЛЮЧЕНЬ</b>\n\n";

  msg += "📅 На <b>" + nowDateUa() + "</b>\n\n";

  String definite = schedule;
  String possible;
  const String definitePrefix = "Світла не буде: ";
  const String possiblePrefix = "\nМожливі відключення: ";

  int p = definite.indexOf(definitePrefix);
  if (p >= 0) definite = definite.substring(p + definitePrefix.length());

  int q = definite.indexOf(possiblePrefix);
  if (q >= 0) {
    possible = definite.substring(q + possiblePrefix.length());
    definite = definite.substring(0, q);
  }

  msg += "🔴 Світла не буде:\n<b>" + definite + "</b>";

  if (possible.length()) {
    msg += "\n\n🟡 Можливі відключення:\n<b>" + possible + "</b>";
  }

  msg += "\n\n🕒 Останнє оновлення ДТЕК: " + update;
  return msg;
}

int minuteOfDay() {
  if (!Clock::isTimeValid()) return -1;
  time_t now = Clock::timestamp(); struct tm t;
  if (!localtime_r(&now, &t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}
String localDateEpochKey() {
  time_t now = time(nullptr); struct tm t;
  if (now < 1704067200 || !localtime_r(&now, &t)) return "";
  t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
  time_t midnight = mktime(&t);
  return String((uint32_t)midnight);
}

String decodeUrl(const String& s) {
  String out; out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] == '%' && i + 2 < s.length()) {
      char h[3] = {s[i + 1], s[i + 2], 0}; out += (char)strtol(h, nullptr, 16); i += 2;
    } else if (s[i] == '+') out += ' ';
    else out += s[i];
  }
  return out;
}
String base64DecodeString(const String& input) {
  String s = input; s.replace("-", "+"); s.replace("_", "/");
  while (s.length() % 4) s += '=';
  size_t olen = 0;
  unsigned char* out = (unsigned char*)malloc(s.length() * 3 / 4 + 8);
  if (!out) return "";
  int rc = mbedtls_base64_decode(out, s.length() * 3 / 4 + 8, &olen,
                                 (const unsigned char*)s.c_str(), s.length());
  String result = rc == 0 ? String((char*)out).substring(0, olen) : "";
  free(out); return result;
}
String makeMaskedCsrf(const String& token) {
  if (!token.length()) return "";
  size_t n = token.length();
  uint8_t* bytes = (uint8_t*)malloc(n * 2);
  if (!bytes) return "";
  for (size_t i = 0; i < n; ++i) bytes[i] = (uint8_t)random(0, 256);
  for (size_t i = 0; i < n; ++i) bytes[n + i] = bytes[i] ^ (uint8_t)token[i];
  size_t olen = 0;
  unsigned char* out = (unsigned char*)malloc(n * 3 + 8);
  if (!out) { free(bytes); return ""; }
  int rc = mbedtls_base64_encode(out, n * 3 + 8, &olen, bytes, n * 2);
  String result = rc == 0 ? String((char*)out).substring(0, olen) : "";
  result.replace("+", "-"); result.replace("/", "_"); result.replace("=", "");
  free(bytes); free(out); return result;
}
String extractCookieToken(const String& setCookie) {
  int p = setCookie.indexOf("_csrf-dtek-krem="); if (p < 0) return "";
  p += strlen("_csrf-dtek-krem="); int e = setCookie.indexOf(';', p);
  String value = decodeUrl(setCookie.substring(p, e < 0 ? setCookie.length() : e));
  int q1 = value.indexOf("\""); if (q1 < 0) return "";
  int q2 = value.indexOf("\"", q1 + 1); if (q2 < 0) return "";
  return value.substring(q1 + 1, q2);
}
String extractCookieValue(const String& setCookie) {
  int p = setCookie.indexOf("_csrf-dtek-krem="); if (p < 0) return "";
  p += strlen("_csrf-dtek-krem="); int e = setCookie.indexOf(';', p);
  return setCookie.substring(p, e < 0 ? setCookie.length() : e);
}

bool postDtek(const String& body, const String& cookie, const String& csrfHeader, String& response, String& setCookie) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(15000); static const char* responseHeaders[] = {"Set-Cookie"};
  http.collectHeaders(responseHeaders, 1);
  if (!http.begin(client, DTEK_AJAX)) return false;
  http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130.0.0.0 Safari/537.36");
  http.addHeader("Accept", "application/json, text/javascript, */*; q=0.01");
  http.addHeader("X-Requested-With", "XMLHttpRequest");
  http.addHeader("Origin", DTEK_BASE); http.addHeader("Referer", DTEK_REFERER);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
  if (cookie.length()) http.addHeader("Cookie", "_csrf-dtek-krem=" + cookie);
  if (csrfHeader.length()) http.addHeader("X-CSRF-Token", csrfHeader);
  int code = http.POST(body);
  setCookie = http.header("Set-Cookie");
  response = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("[DTEK] POST HTTP %d, response bytes=%u\n", code, (unsigned)response.length());
  return code == 200 && response.length() > 0;
}

bool fetchSchedule(String& schedule, String& factUpdate, String& error) {
  // Fetch the already-parsed public JSON from our GitHub repository.
  static const char* SCHEDULE_URL =
    "https://raw.githubusercontent.com/alexx-gnom/blackout-to-tg/main/schedule.json";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, SCHEDULE_URL)) {
    error = "Не вдалося розпочати HTTPS-запит до GitHub";
    return false;
  }
  http.addHeader("Cache-Control", "no-cache");
  int code = http.GET();
  String response = code > 0 ? http.getString() : "";
  http.end();
  Serial.printf("[GITHUB] GET HTTP %d, response bytes=%u\n", code, (unsigned)response.length());

  if (code != HTTP_CODE_OK || response.length() == 0) {
    error = "GitHub не повернув schedule.json (HTTP " + String(code) + ")";
    return false;
  }

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, response);
  if (de) {
    error = String("JSON GitHub не розібрався: ") + de.c_str();
    return false;
  }
  if (!(doc["ok"] | false)) {
    error = "У schedule.json поле ok не true";
    return false;
  }

  String jsonDate = doc["date"] | "";
  String today = nowDate();
  if (!today.length() || jsonDate != today) {
    error = "schedule.json не за сьогодні (дата: " + jsonDate + ")";
    return false;
  }
  String group = doc["group"] | "";
  if (group != GROUP) {
    error = "Невірна черга в schedule.json: " + group;
    return false;
  }

  factUpdate = doc["updated"] | "";
  if (!factUpdate.length()) {
    error = "У schedule.json немає часу оновлення ДТЕК";
    return false;
  }

  auto formatIntervals = [](JsonVariantConst array) -> String {
    String out;
    if (!array.is<JsonArrayConst>()) return out;
    for (JsonObjectConst item : array.as<JsonArrayConst>()) {
      String a = item["start"] | "";
      String b = item["end"] | "";
      if (!a.length() || !b.length()) continue;
      if (out.length()) out += ", ";
      out += a + "–" + b;
    }
    return out;
  };

  String definite = formatIntervals(doc["outages"]);
  String possible = formatIntervals(doc["possibleOutages"]);
  schedule = "Світла не буде: " + (definite.length() ? definite : String("підтверджених відключень немає"));
  if (possible.length()) schedule += "\nМожливі відключення: " + possible;

  Serial.printf("[GITHUB] Дата=%s, група=%s, оновлення ДТЕК=%s\n",
                jsonDate.c_str(), group.c_str(), factUpdate.c_str());
  Serial.println(schedule);
  return true;
}

bool sendTelegram(const String& text) {
  if (String(TELEGRAM_BOT_TOKEN).startsWith("PUT_") || String(TELEGRAM_CHAT_ID).startsWith("PUT_")) {
    Serial.println("[TG] Заповни TELEGRAM_BOT_TOKEN і TELEGRAM_CHAT_ID у secrets.h"); return false;
  }
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc; doc["chat_id"] = TELEGRAM_CHAT_ID; doc["text"] = text; doc["parse_mode"] = "HTML";
  String body; serializeJson(doc, body);
  int code = http.POST(body); String response = http.getString(); http.end();
  bool ok = code == 200 && response.indexOf("\"ok\":true") >= 0;
  Serial.printf("[TG] send HTTP %d: %s\n", code, ok ? "OK" : response.c_str());
  return ok;
}

bool getTelegramUpdates(JsonDocument& doc) {
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(8000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/getUpdates?timeout=0&offset=" + tgOffset;
  if (!http.begin(client, url)) return false;
  int code = http.GET(); String body = http.getString(); http.end();
  if (code != 200) return false;
  return !deserializeJson(doc, body);
}
void checkTelegramCommands() {
  if (millis() - lastTelegramMs < TELEGRAM_POLL_MS) return;
  lastTelegramMs = millis();
  if (WiFi.status() != WL_CONNECTED || String(TELEGRAM_BOT_TOKEN).startsWith("PUT_")) return;
  JsonDocument doc; if (!getTelegramUpdates(doc)) return;
  for (JsonObject update : doc["result"].as<JsonArray>()) {
    long id = update["update_id"] | 0L; if (id >= tgOffset.toInt()) { tgOffset = String(id + 1); prefs.putString("tgOffset", tgOffset); }
    String chat = update["message"]["chat"]["id"].as<String>();
    String text = update["message"]["text"] | ""; text.trim();
    if (chat != String(TELEGRAM_CHAT_ID)) continue;
    if (text == "/schedule" || text.startsWith("/schedule@")) {
      String s, u, e;
      if (fetchSchedule(s, u, e)) sendTelegram(makeReportMessage(s, u));
      else sendTelegram("⚠️ Не вдалося отримати свіжий графік ДТЕК: " + e + ". Попередній графік не вважається актуальним.");
    } else if (text == "/start" || text == "/help") {
      sendTelegram("Бот графіків ДТЕК, Ірпінь, черга 6.1.\n/schedule — отримати графік зараз.");
    }
  }
}

bool checkSchedule(bool startupReport = false) {
  String today = nowDate();
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
                        minute >= FIRST_REPORT_MINUTE &&
                        minute <= LAST_CHECK_MINUTE;
  const bool changed = lastSchedule.length() &&
                       (schedule != lastSchedule || update != lastFactUpdate);
  bool reportOk = true;

  // Startup report is independent of the daily 08:05 report.
  // If startup happens during the normal reporting window, it also satisfies today's report.
  if (startupReport || dailyDue) {
    String msg = makeReportMessage(schedule, update);
    reportOk = sendTelegram(msg);
    if (reportOk && minute >= FIRST_REPORT_MINUTE && minute <= LAST_CHECK_MINUTE) {
      lastReportDate = today;
      prefs.putString("reportDate", lastReportDate);
    }
  } else if (changed) {
    String msg = makeReportMessage(schedule, update, true);
    reportOk = sendTelegram(msg);
  }

  // Store only after a valid response. A failed change notification is retried next check
  // because we keep the previous schedule in that case.
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
  Serial.println("=== BlackoutToTg START ===");

  prefs.begin("blackoutbot", false);
  lastSchedule = prefs.getString("schedule", "");
  lastReportDate = prefs.getString("reportDate", "");
  lastFactUpdate = prefs.getString("factUpdate", "");
  tgOffset = prefs.getString("tgOffset", "0");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Clock::begin();

  Serial.println("[BOOT] Blackout bot стартував. Чекаю Wi-Fi та синхронізацію часу.");
}

void loop() {
  static uint32_t lastLog = 0;
  const uint32_t nowMs = millis();

  // Clock.update() is non-blocking and must be called even while Wi-Fi is connecting.
  Clock::update();

  if (nowMs - lastLog >= 5000) {
    lastLog = nowMs;
    Serial.printf("[ALIVE] %s | Wi-Fi=%s | Clock=%s\n",
      Clock::dateTime().c_str(),
      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
      Clock::isTimeValid() ? "valid" : "waiting");
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (nowMs - lastWifiMs >= WIFI_RETRY_MS) {
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

  // Send the same full report once after every ESP boot. Retry at most every 30 seconds
  // if DTEK or Telegram is temporarily unavailable.
  if (!bootReportSent &&
      (lastBootReportAttemptMs == 0 || nowMs - lastBootReportAttemptMs >= 30000UL)) {
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
  if (minute >= FIRST_REPORT_MINUTE && minute <= LAST_CHECK_MINUTE) {
    const String today = nowDate();

    // Daily report after 08:05 if not already sent today.
    if (lastReportDate != today &&
        (lastDailyAttemptMs == 0 || nowMs - lastDailyAttemptMs >= 60000UL)) {
      lastDailyAttemptMs = nowMs;
      checkSchedule(false);
    }

    // Poll our GitHub schedule JSON every 5 minutes.
    if (lastCheckMs == 0 || nowMs - lastCheckMs >= CHECK_EVERY_MS) {
      checkSchedule(false);
    }
  }

  delay(10);
}
