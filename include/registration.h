// webOS registration manifest, byte-for-byte the one lgtv-streamdeck and
// lgtvremote-cli use. Client keys are bound to the manifest they were issued
// under, so this must not change or the imported pairings stop working.
#pragma once
#ifdef ARDUINO
#include <Arduino.h>
using LGManifestString = String;
#define LG_FLASH PROGMEM
#define LG_MANIFEST_TEXT FPSTR(LG_MANIFEST)
#else
#include <string>
using LGManifestString = std::string;
#define LG_FLASH
#define LG_MANIFEST_TEXT LG_MANIFEST
#endif

static const char LG_MANIFEST[] LG_FLASH = R"json({"manifestVersion":1,"appVersion":"1.1","signed":{"created":"20140509","appId":"com.lge.test","vendorId":"com.lge","localizedAppNames":{"":"LG Remote App","ko-KR":"리모컨 앱","zxx-XX":"ЛГ Rэмotэ AПП"},"localizedVendorNames":{"":"LG Electronics"},"permissions":["TEST_SECURE","CONTROL_INPUT_TEXT","CONTROL_MOUSE_AND_KEYBOARD","READ_INSTALLED_APPS","READ_LGE_SDX","READ_NOTIFICATIONS","SEARCH","WRITE_SETTINGS","WRITE_NOTIFICATION_ALERT","CONTROL_POWER","READ_CURRENT_CHANNEL","READ_RUNNING_APPS","READ_UPDATE_INFO","UPDATE_FROM_REMOTE_APP","READ_LGE_TV_INPUT_EVENTS","READ_TV_CURRENT_TIME"],"serial":"2f930e2d2cfe083771f68e4fe7bb07"},"permissions":["LAUNCH","LAUNCH_WEBAPP","APP_TO_APP","CLOSE","TEST_OPEN","TEST_PROTECTED","CONTROL_AUDIO","CONTROL_DISPLAY","CONTROL_INPUT_JOYSTICK","CONTROL_INPUT_MEDIA_RECORDING","CONTROL_INPUT_MEDIA_PLAYBACK","CONTROL_INPUT_TV","CONTROL_MOUSE_AND_KEYBOARD","CONTROL_INPUT_TEXT","CONTROL_POWER","READ_APP_STATUS","READ_CURRENT_CHANNEL","READ_INPUT_DEVICE_LIST","READ_NETWORK_STATE","READ_RUNNING_APPS","READ_TV_CHANNEL_LIST","WRITE_NOTIFICATION_TOAST","READ_POWER_STATE","READ_COUNTRY_INFO","READ_SETTINGS","CONTROL_TV_SCREEN","CONTROL_TV_STANBY","CONTROL_FAVORITE_GROUP","CONTROL_USER_INFO","CHECK_BLUETOOTH_DEVICE","CONTROL_BLUETOOTH","CONTROL_TIMER_INFO","STB_INTERNAL_CONNECTION","CONTROL_RECORDING","READ_RECORDING_STATE","WRITE_RECORDING_LIST","READ_RECORDING_LIST","READ_RECORDING_SCHEDULE","WRITE_RECORDING_SCHEDULE"],"signatures":[{"signatureVersion":1,"signature":"eyJhbGdvcml0aG0iOiJSU0EtU0hBMjU2Iiwia2V5SWQiOiJ0ZXN0LXNpZ25pbmctY2VydCIsInNpZ25hdHVyZVZlcnNpb24iOjF9.hrVRgjCwXVvE2OOSpDZ58hR+59aFNwYDyjQgKk3auukd7pcegmE2CzPCa0bJ0ZsRAcKkCTJrWo5iDzNhMBWRyaMOv5zWSrthlf7G128qvIlpMT0YNY+n/FaOHE73uLrS/g7swl3/qH/BGFG2Hu4RlL48eb3lLKqTt2xKHdCs6Cd4RMfJPYnzgvI4BNrFUKsjkcu+WD4OO2A27Pq1n50cMchmcaXadJhGrOqH5YmHdOCj5NSHzJYrsW0HPlpuAx/ECMeIZYDh6RMqaFM2DXzdKX9NmmyqzJ3o/0lkk/N97gfVRLW5hA29yeAwaCViZNCP8iC9aO0q9fQojoa7NQnAtw=="}]})json";

// Keep every permission when removing the certificate rejected by webOS 26.
// Used by both the remote and the app picker's private socket.
#include <ArduinoJson.h>
inline LGManifestString lgManifest(bool unsignedRegistration) {
  if (!unsignedRegistration) return LG_MANIFEST_TEXT;
  JsonDocument doc;
  deserializeJson(doc, LG_MANIFEST_TEXT);
  JsonArray permissions = doc["permissions"].as<JsonArray>();
  for (JsonVariantConst permission : doc["signed"]["permissions"].as<JsonArrayConst>()) {
    bool found = false;
    for (JsonVariantConst existing : permissions) {
      if (!strcmp(existing.as<const char*>(), permission.as<const char*>())) { found = true; break; }
    }
    if (!found) permissions.add(permission.as<const char*>());
  }
  doc.remove("signed");
  doc.remove("signatures");
  LGManifestString result;
  serializeJson(doc, result);
  return result;
}

inline bool lgShouldRetryUnsigned(const char* error, bool registrationMatches,
                                 bool unsignedMode, bool consentStarted, bool pinSubmitted) {
  return registrationMatches && !unsignedMode && !consentStarted && !pinSubmitted && error &&
      !strcmp(error, "403 Pairing rejected: blacklisted certificate detected");
}
