// One SSAP connection to one webOS TV, ported from lgtv-streamdeck/src/lg.
//
// Runs entirely on its own FreeRTOS task so a TV that is off (and therefore
// makes TCP connects block for seconds) never stalls the touch UI. The UI
// posts commands via a queue and reads state through the atomics below.
#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <functional>
#include <atomic>
#include "tvstore.h"

enum class LinkState : uint8_t { NoWifi, NoTV, Connecting, NeedsPin, Registered };

enum class CmdType : uint8_t {
  None,
  PowerToggle,   // getPowerState -> turnOff, else Wake-on-LAN (plugin semantics)
  Request,       // a = ssap uri, b = json payload or ""
  Button,        // a = pointer-socket button name (UP, ENTER, BACK...)
  MuteToggle,    // getVolume -> setMute(!muted)
  PlayPause,     // getForegroundAppInfo -> play/pause
  ScreenToggle,  // turnOffScreen / turnOnScreen with luna fallback
  LaunchApp,     // a = app id
  SelectTV,      // a = store index
  Scan,          // SSDP discovery; results in found list
  PairStart,     // a = ip, b = name; registers without a key so the TV shows a PIN
  SubmitPin,     // a = pin
  CancelPair,
  Forget,        // a = store index
  FetchApps,     // ask the TV for its installed apps (for the web editor)
};

struct Cmd {
  CmdType type = CmdType::None;
  char a[96] = {0};
  char b[160] = {0};
};

struct FoundTV { char name[32]; char ip[16]; };
struct InputInfo { char id[16]; char label[24]; bool connected; };
struct AppInfo { char id[40]; char title[32]; };

class LGTV {
 public:
  void begin(TVStore* store);
  bool post(CmdType t, const char* a = nullptr, const char* b = nullptr);

  // Shared state for the UI (written on the net task).
  std::atomic<LinkState> link{LinkState::NoWifi};
  std::atomic<bool> muted{false};
  std::atomic<bool> playing{false};
  std::atomic<bool> screenOff{false};
  std::atomic<bool> pairing{false};      // a PairStart is in progress
  std::atomic<bool> pairFailed{false};   // last PIN was rejected (cleared by next SubmitPin)
  std::atomic<bool> scanning{false};
  std::atomic<uint32_t> foundGen{0};     // bumps when the found list changes
  std::atomic<uint32_t> lastActivity{0};
  std::atomic<uint32_t> lastError{0};

  int foundCount();
  bool getFound(int i, FoundTV& out);

  // External inputs as the TV reports them (label = the user's name for it).
  std::atomic<uint32_t> inputsGen{0};
  bool getInput(const char* id, InputInfo& out);
  int inputCount();
  bool getInputAt(int i, InputInfo& out);

  // Installed apps as the TV reports them (fetched on demand).
  std::atomic<uint32_t> appsGen{0};
  std::atomic<bool> appsLoading{false};
  int appCount();
  bool getAppAt(int i, AppInfo& out);

 private:
  using ResponseCb = std::function<void(bool ok, JsonObjectConst payload)>;
  struct Pending { uint32_t id = 0; uint32_t deadline = 0; ResponseCb cb; };

  static void taskEntry(void* self);
  void task();
  void handleCmd(const Cmd& c);
  void connectMain();
  void dropAll();
  void onMainEvent(WStype_t type, uint8_t* data, size_t len);
  void onPointerEvent(WStype_t type, uint8_t* data, size_t len);
  void sendRegister();
  void onRegistered(const char* key);
  void fetchMacs(bool secondTry);
  void fetchInputs();
  uint32_t request(const char* uri, const char* payloadJson, ResponseCb cb, uint32_t timeoutMs = 4000);
  void expirePending();
  void wake();
  void openPointerSocket();
  void sendButtonRaw(const char* name);
  void lunaCall(const char* lunaUri, const char* paramsJson);
  void scan();

  TVStore* store_ = nullptr;
  TVRecord cur_ = {};
  int curIdx_ = -1;
  QueueHandle_t queue_ = nullptr;
  WebSocketsClient ws_;
  WebSocketsClient ptr_;
  bool wsBegun_ = false;
  bool ptrBegun_ = false;
  bool ptrOpen_ = false;
  bool useTls_ = true;
  bool unsignedRegistration_ = false;
  bool unsignedRetryPending_ = false;
  bool registrationBlocked_ = false;
  bool consentStarted_ = false;
  bool pinSubmitted_ = false;
  bool registrationAnswered_ = false;
  uint32_t registrationDeadline_ = 0;
  uint32_t pinRequestId_ = 0;
  String registrationId_;
  uint8_t failStreak_ = 0;
  uint32_t nextId_ = 1;
  static const int MAX_PENDING = 6;
  Pending pending_[MAX_PENDING];
  char pendingButton_[16] = {0};
  static const uint32_t PIN_REQUEST_ID = 0xFFFFFFF0;

  SemaphoreHandle_t foundMtx_ = nullptr;
  static const int MAX_FOUND = 8;
  FoundTV found_[MAX_FOUND];
  int foundCount_ = 0;
  static const int MAX_INPUTS = 8;
  InputInfo inputs_[MAX_INPUTS];
  int inputCount_ = 0;
  static const int MAX_APPS = 80;
  AppInfo* apps_ = nullptr;   // allocated on first fetch
  int appCount_ = 0;
  void fetchApps();
};
