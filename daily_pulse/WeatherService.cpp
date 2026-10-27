#include "WeatherService.h"
#include "Clock.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

namespace {
constexpr char WEATHER_URL[] =
  "https://api.open-meteo.com/v1/forecast?latitude=50.5218&longitude=30.2506"
  "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
  "&daily=temperature_2m_max,temperature_2m_min,weather_code"
  "&hourly=temperature_2m,weather_code,wind_gusts_10m"
  "&timezone=Europe%2FKyiv&forecast_days=2";

String formatTemperature(float t) {
  int rounded = (int)roundf(t);
  return String(rounded > 0 ? "+" : "") + String(rounded) + "°C";
}
String timeText(int hour) {
  if (hour >= 24) hour -= 24;
  return (hour < 10 ? "0" : "") + String(hour) + ":00";
}
String conditions(int c) {
  if (c == 0) return "Ясно ☀️";
  if (c == 1) return "Переважно ясно 🌤";
  if (c == 2) return "Мінлива хмарність ⛅";
  if (c == 3) return "Хмарно ☁️";
  if (c == 45 || c == 48) return "Туман 🌫";
  if (c >= 51 && c <= 57) return "Мряка 🌦";
  if (c >= 61 && c <= 67) return "Дощ 🌧";
  if (c >= 71 && c <= 77) return "Сніг ❄️";
  if (c >= 80 && c <= 82) return "Зливи 🌧";
  if (c == 85 || c == 86) return "Снігопад 🌨";
  if (c >= 95 && c <= 99) return "Гроза ⛈";
  return "Мінлива погода 🌤";
}
int precipitationKind(int code) {
  if (code >= 95 && code <= 99) return 4;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 3;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return 2;
  if (code >= 51 && code <= 57) return 1;
  return 0;
}
int precipitationIntensity(int code) {
  switch (code) {
    case 55: case 57: case 65: case 67: case 75: case 82: case 86: case 99: return 3;
    case 53: case 63: case 66: case 73: case 81: case 95: case 96: return 2;
    default: return 1;
  }
}
String precipitationLabel(int code) {
  int kind = precipitationKind(code);
  int strength = precipitationIntensity(code);
  if (kind == 1) return strength == 3 ? "🌧 Сильна мряка" : "🌦 Мряка";
  if (kind == 2) {
    if (code == 66 || code == 67) return "🌧 Крижаний дощ";
    if (strength == 1) return "🌦 Невеликий дощ";
    if (strength == 3) return "🌧 Сильний дощ";
    return "🌧 Помірний дощ";
  }
  if (kind == 3) {
    if (strength == 1) return "🌨 Невеликий сніг";
    if (strength == 3) return "🌨 Сильний сніг";
    return "🌨 Сніг";
  }
  return "⛈ Гроза";
}
String windLabel(float speed) {
  if (speed <= 5) return "Майже безвітряно";
  if (speed <= 11) return "Легкий вітер";
  if (speed <= 19) return "Помірний вітер";
  if (speed <= 28) return "Відчутний вітер";
  if (speed <= 38) return "Сильний вітер";
  if (speed <= 49) return "Дуже сильний вітер";
  if (speed <= 61) return "Штормовий вітер";
  return "Небезпечний вітер";
}

// Hourly WMO code applies to the period [HH:00, HH+1:00).
// One block per uninterrupted sequence of the same precipitation type.
String precipitationPeriods(JsonArray times, JsonArray codes,
                            const String& day0, const String& day1,
                            int dayStartHour, bool night) {
  String result;
  int start = -1, end = -1, type = 0, peakCode = 0, peakIntensity = 0;
  auto flush = [&]() {
    if (start < 0) return;
    if (result.length()) result += "\n";
    result += precipitationLabel(peakCode) + " очікується з " +
              timeText(start) + " до " + timeText(end) + ".";
    start = -1;
  };
  for (size_t i = 0; i <= times.size(); ++i) {
    String stamp = (i < times.size()) ? times[i].as<String>() : String();
    int hh = stamp.length() >= 16 ? stamp.substring(11, 13).toInt() : -1;
    bool inWindow = night
      ? ((stamp.startsWith(day0) && hh >= 21) || (stamp.startsWith(day1) && hh < 9))
      : (stamp.startsWith(day0) && hh >= dayStartHour);
    int code = (inWindow && i < codes.size()) ? codes[i].as<int>() : 0;
    int nextType = inWindow ? precipitationKind(code) : 0;
    // Night can cross midnight: display 24+ hours (e.g. 02:00 = 26:00)
    // only for internal continuity, format as a local clock below.
    int hourPos = stamp.startsWith(day1) ? hh + 24 : hh;
    if (start >= 0 && (nextType != type || hourPos != end)) flush();
    if (nextType != 0) {
      if (start < 0) {
        start = hourPos;
        type = nextType;
        peakCode = code;
        peakIntensity = precipitationIntensity(code);
      } else if (precipitationIntensity(code) > peakIntensity) {
        peakCode = code;
        peakIntensity = precipitationIntensity(code);
      }
      end = hourPos + 1;
    }
  }
  // Normalize night-local hours in output: 24:00 or later -> 00:00 etc.
  // Strings above are built with timeText(); it accepts extended hours below.
  return result;
}
}

bool fetchWeatherMessage(String& message, String& error) {
  if (!Clock::isTimeValid()) { error = "Clock not synchronized"; return false; }
  WiFiClientSecure client;
  client.setInsecure(); // Preview firmware: HTTPS cert validation can be added before deployment.
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, WEATHER_URL)) { error = "HTTP begin failed"; return false; }
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    error = "Open-Meteo HTTP " + String(status);
    http.end(); return false;
  }
String response = http.getString();
http.end();

Serial.printf(
    "[WEATHER] Response size: %u bytes\n",
    (unsigned)response.length()
);

JsonDocument doc;
DeserializationError parse = deserializeJson(doc, response);

if (parse) {
    error = String("Invalid JSON: ") + parse.c_str();
    Serial.println("[WEATHER] Response preview:");
    Serial.println(response.substring(0, 300));
    return false;
}

  JsonVariant current = doc["current"];
  JsonVariant daily = doc["daily"];
  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray temps = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray codes = doc["hourly"]["weather_code"].as<JsonArray>();
  JsonArray gusts = doc["hourly"]["wind_gusts_10m"].as<JsonArray>();
  if (current["temperature_2m"].isNull() || current["weather_code"].isNull() ||
      current["wind_speed_10m"].isNull() || daily["temperature_2m_min"][0].isNull() ||
      daily["temperature_2m_max"][0].isNull() || daily["time"][1].isNull() ||
      times.size() < 34 || times.size() != temps.size() ||
      times.size() != codes.size() || times.size() != gusts.size()) {
    error = "Missing hourly/daily fields"; return false;
  }

  String today = daily["time"][0].as<String>();
  String tomorrow = daily["time"][1].as<String>();
  if (today != Clock::date()) { error = "Weather day mismatch: " + today; return false; }
  String curTime = current["time"].as<String>();
  int nowHour = curTime.length() >= 13 ? curTime.substring(11, 13).toInt() : 7;
  int startHour = nowHour > 7 ? nowHour : 7;

  message = "🌤 Прогноз погоди — Ірпінь\n\n";
  message += conditions(current["weather_code"].as<int>()) + "\n";
  message += "🌡 Зараз " + formatTemperature(current["temperature_2m"].as<float>());
  if (!current["apparent_temperature"].isNull())
    message += " (відчувається як " + formatTemperature(current["apparent_temperature"].as<float>()) + ")";
  message += "\n📈 Протягом дня " + formatTemperature(daily["temperature_2m_min"][0].as<float>()) +
             "…" + formatTemperature(daily["temperature_2m_max"][0].as<float>()) + "\n\n";

  String dayRain = precipitationPeriods(times, codes, today, tomorrow, startHour, false);
  message += dayRain.length() ? dayRain : "☀️ Опадів до кінця дня не прогнозують.";
  message += "\n\n";

  float speed = current["wind_speed_10m"].as<float>();
  message += "💨 " + windLabel(speed) + ", " + String((int)roundf(speed)) + " км/год.";
  float maxGust = 0;
  for (size_t i = 0; i < times.size(); ++i) {
    String stamp = times[i].as<String>();
    if (!stamp.startsWith(today) || stamp.length() < 13 ||
        stamp.substring(11, 13).toInt() < startHour || gusts[i].isNull()) continue;
    float gust = gusts[i].as<float>();
    if (gust > maxGust) maxGust = gust;
  }
  if (maxGust >= 30 && maxGust >= speed * 1.5f)
    message += " Пориви до " + String((int)roundf(maxGust)) + " км/год.";

  bool foundNight = false;
  float minNight = 1000, maxNight = -1000;
  for (size_t i = 0; i < times.size(); ++i) {
    String stamp = times[i].as<String>();
    if (stamp.length() < 13 || temps[i].isNull()) continue;
    int hh = stamp.substring(11, 13).toInt();
    bool nextNight = (stamp.startsWith(today) && hh >= 21) ||
                     (stamp.startsWith(tomorrow) && hh < 9);
    if (!nextNight) continue;
    float t = temps[i].as<float>();
    if (t < minNight) minNight = t;
    if (t > maxNight) maxNight = t;
    foundNight = true;
  }
  if (!foundNight) { error = "Night temperatures missing"; return false; }
  message += "\n\n🌙 Наступна ніч (21:00–09:00)\n";
  message += "🌡 Температура: " + formatTemperature(minNight) + "…" + formatTemperature(maxNight) + "\n";
  String nightRain = precipitationPeriods(times, codes, today, tomorrow, 21, true);
  message += nightRain.length() ? nightRain + "\n" : "🌙 Опадів уночі не прогнозують.\n";
  if (minNight <= 0.0f)
    message += "❄️ Прогнозується мороз! Варто захистити рослини.\n";
  else if (minNight <= 2.0f)
    message += "⚠️ Можливі приземні заморозки. Варто накрити чутливі рослини.\n";
  message += "\nДжерело: Open-Meteo";
  return true;
}
