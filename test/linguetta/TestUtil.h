#pragma once

#include <ArduinoJson.h>

#include <fstream>
#include <sstream>
#include <string>

inline std::string fixturePath(const std::string& name) { return std::string(LINGUETTA_FIXTURES) + "/" + name; }

inline std::string readFixture(const std::string& name) {
  std::ifstream in(fixturePath(name), std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline JsonDocument loadJsonFixture(const std::string& name) {
  JsonDocument doc;
  const std::string text = readFixture(name);
  deserializeJson(doc, text);
  return doc;
}
