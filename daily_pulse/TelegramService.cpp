#include "TelegramService.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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

bool getTelegramUpdates(JsonDocument& doc, const String& botToken, const String& offset) {
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(8000);
  String url = String("https://api.telegram.org/bot") + botToken + "/getUpdates?timeout=0&offset=" + offset;
  if (!http.begin(client, url)) return false;
  int code = http.GET(); String body = http.getString(); http.end();
  if (code != 200) return false;
  return !deserializeJson(doc, body);
}

// Telegram fetches the image from GitHub itself, so the ESP never buffers JPG bytes.
bool sendTelegramPhoto(const String& imageUrl, const String& caption) {
  if (String(TELEGRAM_BOT_TOKEN).startsWith("PUT_") || String(TELEGRAM_CHAT_ID).startsWith("PUT_")) return false;
  WiFiClientSecure client;
  client.setInsecure(); // Prototype HTTPS behavior, same as existing Telegram calls.
  HTTPClient http;
  http.setTimeout(20000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendPhoto";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["chat_id"] = TELEGRAM_CHAT_ID;
  doc["photo"] = imageUrl;
  doc["caption"] = caption;
  String body;
  serializeJson(doc, body);
  const int code = http.POST(body);
  String response = http.getString();
  http.end();
  bool ok = code == 200 && response.indexOf("\"ok\":true") >= 0;
  Serial.printf("[TG] sendPhoto HTTP %d: %s\n", code, ok ? "OK" : response.c_str());
  return ok;
}
