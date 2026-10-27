#pragma once
#include <Arduino.h>

String nowDateUa();
int minuteOfDay();
bool fetchSchedule(String& schedule, String& factUpdate, String& error);
String makeReportMessage(const String& schedule, const String& update, bool changed = false);
