#include "DailyContentService.h"
#include "Config.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

bool fetchDailyContent(const String& expectedDate, DailyContent& result, String& error) {
  WiFiClientSecure client;
  client.setInsecure(); // Prototype: use certificate validation in production.
  HTTPClient http;
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, Config::DAILY_URL)) {
    error = "HTTP begin failed";
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    error = "HTTP " + String(code);
    http.end();
    return false;
  }
  // The published JSON is small; ArduinoJson v7 JsonDocument is dynamic.
  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, http.getStream());
  http.end();
  if (jsonError) {
    error = String("JSON: ") + jsonError.c_str();
    return false;
  }
  if ((doc["version"] | 0) != 1) {
    error = "Unsupported JSON version";
    return false;
  }
  String date = doc["date"].as<String>();
  if (date != expectedDate) {
    error = "Stale/future content: " + date + " (expected " + expectedDate + ")";
    return false;
  }
  DailyContent temp;
  temp.date = date;
  temp.morningCaption = doc["morning"]["caption"].as<String>();
  temp.morningExtra = doc["morning"]["extra_text"].as<String>();
  temp.morningImageUrl = doc["morning"]["image_url"].as<String>();
  temp.eveningCaption = doc["evening"]["caption"].as<String>();
  temp.eveningImageUrl = doc["evening"]["image_url"].as<String>();
  if (temp.morningCaption.isEmpty() || temp.morningImageUrl.isEmpty() ||
      temp.eveningCaption.isEmpty() || temp.eveningImageUrl.isEmpty()) {
    error = "Missing message/photo field";
    return false;
  }
  if (!temp.morningImageUrl.startsWith("https://") ||
      !temp.eveningImageUrl.startsWith("https://")) {
    error = "Image URL must be HTTPS";
    return false;
  }
  result = temp;
  return true;
}
