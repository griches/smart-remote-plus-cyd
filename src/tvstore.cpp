#include "tvstore.h"
#include "log.h"
#include <Preferences.h>
#include <ArduinoJson.h>

#if __has_include("tv_config.h")
#include "tv_config.h"
#define HAVE_SEED 1
#endif

static const char* NS = "lgtv";

void TVStore::begin() {
  mtx_ = xSemaphoreCreateMutex();
  Preferences prefs;
  prefs.begin(NS, true);
  String json = prefs.getString("tvs", "");
  selected_ = prefs.getInt("sel", -1);
  prefs.end();

  count_ = 0;
  if (json.length()) {
    JsonDocument doc;
    if (!deserializeJson(doc, json)) {
      for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        if (count_ >= MAX) break;
        TVRecord& r = recs_[count_++];
        strlcpy(r.name, o["name"] | "LG TV", sizeof(r.name));
        strlcpy(r.ip, o["ip"] | "", sizeof(r.ip));
        strlcpy(r.clientKey, o["key"] | "", sizeof(r.clientKey));
        strlcpy(r.mac, o["mac"] | "", sizeof(r.mac));
        strlcpy(r.wifiMac, o["wmac"] | "", sizeof(r.wifiMac));
        r.unsignedRegistration = o["unsigned"] | false;
      }
    }
  }
#ifdef HAVE_SEED
  if (count_ == 0) {
    for (int i = 0; i < TV_COUNT && count_ < MAX; i++) {
      TVRecord& r = recs_[count_++];
      strlcpy(r.name, TVS[i].name, sizeof(r.name));
      strlcpy(r.ip, TVS[i].ip, sizeof(r.ip));
      strlcpy(r.clientKey, TVS[i].clientKey, sizeof(r.clientKey));
      strlcpy(r.mac, TVS[i].mac, sizeof(r.mac));
      strlcpy(r.wifiMac, TVS[i].wifiMac, sizeof(r.wifiMac));
#ifdef TV_SEED_HAS_UNSIGNED
      r.unsignedRegistration = TVS[i].unsignedRegistration;
#endif
    }
    selected_ = count_ ? 0 : -1;
    LOGF("[store] seeded %d TVs from tv_config.h\n", count_);
    save();
  }
#endif
  if (selected_ >= count_) selected_ = count_ ? 0 : -1;
  if (selected_ < 0 && count_) selected_ = 0;
  LOGF("[store] %d TVs, selected %d\n", count_, selected_);
}

void TVStore::save() {
  // caller holds mtx_
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count_; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = recs_[i].name;
    o["ip"] = recs_[i].ip;
    o["key"] = recs_[i].clientKey;
    o["mac"] = recs_[i].mac;
    o["wmac"] = recs_[i].wifiMac;
    o["unsigned"] = recs_[i].unsignedRegistration;
  }
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin(NS, false);
  prefs.putString("tvs", json);
  prefs.putInt("sel", selected_);
  prefs.end();
  gen_++;
}

int TVStore::count() {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  int n = count_;
  xSemaphoreGive(mtx_);
  return n;
}

bool TVStore::get(int i, TVRecord& out) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  bool ok = i >= 0 && i < count_;
  if (ok) out = recs_[i];
  xSemaphoreGive(mtx_);
  return ok;
}

int TVStore::findByIp(const char* ip) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  int r = -1;
  for (int i = 0; i < count_; i++) if (!strcmp(recs_[i].ip, ip)) { r = i; break; }
  xSemaphoreGive(mtx_);
  return r;
}

int TVStore::add(const TVRecord& r) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  int idx = -1;
  if (count_ < MAX) {
    idx = count_++;
    recs_[idx] = r;
    if (selected_ < 0) selected_ = idx;
    save();
  }
  xSemaphoreGive(mtx_);
  return idx;
}

bool TVStore::update(int i, const TVRecord& r) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  bool ok = i >= 0 && i < count_;
  if (ok) { recs_[i] = r; save(); }
  xSemaphoreGive(mtx_);
  return ok;
}

bool TVStore::remove(int i) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  bool ok = i >= 0 && i < count_;
  if (ok) {
    for (int k = i; k < count_ - 1; k++) recs_[k] = recs_[k + 1];
    count_--;
    if (selected_ == i) selected_ = count_ ? 0 : -1;
    else if (selected_ > i) selected_--;
    save();
  }
  xSemaphoreGive(mtx_);
  return ok;
}

int TVStore::selected() {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  int s = selected_;
  xSemaphoreGive(mtx_);
  return s;
}

void TVStore::setSelected(int i) {
  xSemaphoreTake(mtx_, portMAX_DELAY);
  if (i >= 0 && i < count_ && i != selected_) { selected_ = i; save(); }
  xSemaphoreGive(mtx_);
}
