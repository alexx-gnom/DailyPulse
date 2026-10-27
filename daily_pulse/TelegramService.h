#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

bool sendTelegram(const String& text);
bool sendTelegramPhoto(const String& imageUrl, const String& caption);
bool getTelegramUpdates(JsonDocument& doc, const String& botToken, const String& offset);
