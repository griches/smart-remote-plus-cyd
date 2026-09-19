#include "registration.h"
#include <cassert>
#include <set>
#include <iostream>
int main() {
  JsonDocument signedDoc, unsignedDoc;
  assert(!deserializeJson(signedDoc, lgManifest(false)));
  assert(!deserializeJson(unsignedDoc, lgManifest(true)));
  assert(!unsignedDoc["signed"].is<JsonObject>());
  assert(!unsignedDoc["signatures"].is<JsonArray>());
  std::set<std::string> expected, actual;
  for (JsonVariantConst p : signedDoc["permissions"].as<JsonArrayConst>()) expected.insert(p.as<const char*>());
  for (JsonVariantConst p : signedDoc["signed"]["permissions"].as<JsonArrayConst>()) expected.insert(p.as<const char*>());
  for (JsonVariantConst p : unsignedDoc["permissions"].as<JsonArrayConst>()) actual.insert(p.as<const char*>());
  assert(expected == actual);
  for (auto permission : {"CONTROL_INPUT_TEXT", "CONTROL_MOUSE_AND_KEYBOARD", "WRITE_SETTINGS", "WRITE_NOTIFICATION_ALERT", "READ_INSTALLED_APPS"}) assert(actual.count(permission));
  const char* blacklist = "403 Pairing rejected: blacklisted certificate detected";
  assert(lgShouldRetryUnsigned(blacklist, true, false, false, false));
  for (int flags = 0; flags < 16; ++flags) {
    bool expectedRetry = (flags & 1) && !(flags & 14);
    assert(lgShouldRetryUnsigned(blacklist, flags & 1, flags & 2, flags & 4, flags & 8) == expectedRetry);
  }
  for (const char* error : {"403 user denied", "401 invalid key", "403 Pairing rejected", ""}) assert(!lgShouldRetryUnsigned(error, true, false, false, false));
  assert(!lgShouldRetryUnsigned(nullptr, true, false, false, false));
  assert(lgManifest(false) == LG_MANIFEST);
  std::cout << "Manifest permissions, legacy identity and all retry guards passed\n";
}
