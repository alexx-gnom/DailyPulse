#include "ScheduleService.h"
#include "Config.h"
#include "Clock.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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
                         bool changed) {
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
  time_t now = Clock::timestamp();
  struct tm t;
  if (!localtime_r(&now, &t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

bool fetchSchedule(String& schedule, String& factUpdate, String& error) {
  // Fetch the already-parsed public JSON from our GitHub repository.
  const char* SCHEDULE_URL = Config::SCHEDULE_URL;

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
  String today = Clock::date();
  if (!today.length() || jsonDate != today) {
    error = "schedule.json не за сьогодні (дата: " + jsonDate + ")";
    return false;
  }
  String group = doc["group"] | "";
  if (group != Config::GROUP) {
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

