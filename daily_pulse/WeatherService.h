#pragma once
#include <Arduino.h>
// Fetches today's forecast for Irpin (Europe/Kyiv); returns a ready Telegram message.
bool fetchWeatherMessage(String& message, String& error);
