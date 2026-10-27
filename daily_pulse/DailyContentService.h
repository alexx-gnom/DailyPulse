#pragma once
#include <Arduino.h>

struct DailyContent {
  String date;
  String morningCaption;
  String morningExtra;
  String morningImageUrl;
  String eveningCaption;
  String eveningImageUrl;
};

// Downloads small JSON, checks version, mandatory keys and today's date.
bool fetchDailyContent(const String& expectedDate, DailyContent& result, String& error);
