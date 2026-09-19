// Persistent TV records (name, ip, client key, MACs) in the ESP32's NVS
// flash. Thread-safe: the UI (core 1) reads while the network task (core 0)
// writes. Seeded on first boot from include/tv_config.h if that exists.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct TVRecord {
  char name[32];
  char ip[16];
  char clientKey[96];
  char mac[18];
  char wifiMac[18];
  bool unsignedRegistration = false;
};

class TVStore {
 public:
  static const int MAX = 6;
  void begin();
  int count();
  bool get(int i, TVRecord& out);
  int findByIp(const char* ip);
  int add(const TVRecord& r);           // -1 if full
  bool update(int i, const TVRecord& r);
  bool remove(int i);
  int selected();                        // -1 when empty
  void setSelected(int i);
  uint32_t generation() { return gen_; } // bumps on every change, for UI redraws

 private:
  void save();
  SemaphoreHandle_t mtx_ = nullptr;
  TVRecord recs_[MAX] = {};
  int count_ = 0;
  int selected_ = -1;
  volatile uint32_t gen_ = 0;
};
