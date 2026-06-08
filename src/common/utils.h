#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

int abs_int(int value);
int sign_int(int value);
String lower_string(String value);
String json_string_value(const String &json, const char *key);
bool json_has_string_key(const String &json, const char *key);
bool json_has_status(const String &json, const char *status);
String compact_error_detail(const String &body);
bool read_http_line(WiFiClientSecure &client, String &line, unsigned long timeout_ms);
int parse_http_status_code(const String &status_line);
void pump_ui(uint32_t milliseconds);
