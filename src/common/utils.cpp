#include "utils.h"

#include <lvgl.h>

int abs_int(int value) {
  return value < 0 ? -value : value;
}

int sign_int(int value) {
  return (value > 0) - (value < 0);
}

String lower_string(String value) {
  value.toLowerCase();
  return value;
}

String json_string_value(const String &json, const char *key) {
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) {
    return "";
  }
  start += needle.length();
  const int end = json.indexOf('"', start);
  if (end < 0) {
    return "";
  }
  return json.substring(start, end);
}

bool json_has_string_key(const String &json, const char *key) {
  return json.indexOf(String("\"") + key + "\":\"") >= 0;
}

bool json_has_status(const String &json, const char *status) {
  return json.indexOf(String("\"status\":\"") + status + "\"") >= 0;
}

String compact_error_detail(const String &body) {
  String detail = json_string_value(body, "error");
  if (detail.length() == 0) {
    detail = json_string_value(body, "message");
  }
  if (detail.length() == 0) {
    detail = body;
  }
  detail.replace("\n", " ");
  detail.replace("\r", " ");
  detail.trim();
  if (detail.length() > 90) {
    detail = detail.substring(0, 90);
  }
  return detail;
}

bool read_http_line(WiFiClientSecure &client, String &line, unsigned long timeout_ms) {
  line = "";
  const unsigned long started = millis();
  while (millis() - started < timeout_ms) {
    while (client.available()) {
      const char c = client.read();
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        return true;
      }
      if (line.length() < 300) {
        line += c;
      }
    }
    lv_timer_handler();
    delay(5);
  }
  return false;
}

int parse_http_status_code(const String &status_line) {
  const int first_space = status_line.indexOf(' ');
  if (first_space < 0 || first_space + 4 > status_line.length()) {
    return 0;
  }
  return status_line.substring(first_space + 1, first_space + 4).toInt();
}

void pump_ui(uint32_t milliseconds) {
  const uint32_t end_at = millis() + milliseconds;
  while (millis() < end_at) {
    lv_timer_handler();
    delay(5);
  }
}
