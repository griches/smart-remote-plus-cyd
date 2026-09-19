// LG TV remote for the ESP32-2432S028 "Cheap Yellow Display".
//
// A touch port of the lgtv-streamdeck plugin: same actions, same artwork,
// same webOS protocol, same pairings (imported from lgtvremote-cli, or made
// on the device via scan + PIN).
//
// Layout: 4x3 grid of 64px key tiles (80x72 cells) above a 24px status bar.
// Tap the left of the bar (the TV name) to open the TV list, the right of
// the bar to change page.

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "assets.h"
#include "layout.h"
#include "lgtv.h"
#include "log.h"
#include "wifi_creds.h"
#include "tvstore.h"
#include "webui.h"

// ------------------------------------------------------------- hardware

// The Arduino loop task defaults to 8 KB; JSON layouts and the web server want more.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static TFT_eSPI tft;
static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static TVStore store;
static LGTV lg;
SemaphoreHandle_t g_serialMutex;

static const int RAW_MIN = 200, RAW_MAX = 3700;  // touch calibration

// --------------------------------------------------------------- layout

static const int COLS = 4, ROWS = 3;
static const int CELL_W = 80, CELL_H = 72;
static const int GRID_H = ROWS * CELL_H;          // 216
static const int BAR_Y = GRID_H, BAR_H = 24;      // 216..240
static const uint16_t BG = TFT_BLACK;
static const uint16_t BAR_BG = 0x10A2;            // very dark grey
static const uint16_t TILE_BG = 0x18E3;           // dark tile for text buttons
static const uint16_t ACCENT = 0x3D9F;            // blue
static const uint16_t GREEN = 0x2E8B;

static int page = 0;

// Pages and tiles come from layout::current (JSON in NVS, default compiled in).
#define PAGE_COUNT (layout::current.pageCount)
static inline const TileDef& tileAt(int idx) { return layout::current.pages[page].tiles[idx]; }

// ---------------------------------------------------------------- state

enum class Mode : uint8_t { Grid, TVList, Keypad, WifiList, Keyboard };
static Mode mode = Mode::Grid;
static int pressedTile = -1;        // grid: index in current page, -1 = none
static uint32_t pressStart = 0, lastRepeat = 0;

// TV list
static const int ROW_H = 36;
static int confirmForget = -1;      // row index awaiting a second tap on its [x]
static uint32_t confirmUntil = 0;
static uint32_t shownStoreGen = 0, shownFoundGen = 0;

// Keypad
static char pin[9] = {0};
static char pairName[32] = {0};
static char pairIp[16] = {0};
static enum { PK_Connecting, PK_Enter, PK_Checking, PK_Wrong, PK_Done } pkState = PK_Connecting;
static uint32_t pkDoneAt = 0;

// Wi-Fi setup
static WifiCreds creds;
static uint32_t wifiStartedAt = 0;
static int wifiRowCount = 0;          // scan results shown (max 5)
static bool wifiScanShown = false;
static char kbSsid[33] = {0};
static char kbText[65] = {0};
static bool kbShift = false, kbSym = false;

// Backlight
static const int BL_CH = 0;
static uint32_t lastTouchAt = 0;
static bool dimmed = false;
static const uint32_t DIM_AFTER_MS = 60000;

// Snapshots of LGTV state so we redraw only on change.
static LinkState shownLink = LinkState::NoWifi;
static bool shownMuted = false, shownPlaying = false, shownScreenOff = false;
static int shownTV = -2;
static uint32_t shownError = 0;
static uint32_t errorFlashUntil = 0;

// ------------------------------------------------------------- helpers

static bool dynOn(const TileDef& t) {
  switch (t.kind) {
    case TileKind::Mute:      return lg.muted.load();
    case TileKind::PlayPause: return lg.playing.load();
    case TileKind::Screen:    return lg.screenOff.load();
    default:                  return false;
  }
}

static bool tileIsDynamic(const TileDef& t) {
  return t.kind == TileKind::Mute || t.kind == TileKind::PlayPause || t.kind == TileKind::Screen || t.kind == TileKind::Input;
}

static uint16_t linkColour(LinkState ls) {
  switch (ls) {
    case LinkState::Registered: return TFT_GREEN;
    case LinkState::Connecting: return TFT_YELLOW;
    case LinkState::NeedsPin:   return ACCENT;
    case LinkState::NoTV:       return TFT_RED;
    default:                    return TFT_DARKGREY;
  }
}

static void textButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg, int font = 2) {
  tft.fillRoundRect(x, y, w, h, 8, bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(label, x + w / 2, y + h / 2, font);
}

// ---------------------------------------------------------- grid view

// Draw text centred on a blank key, large if it fits, else up to two lines.
static void drawKeyLabel(const char* label, int ix, int iy, bool connectedDot) {

  const int maxW = ICON_W - 8;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(KEY_GLYPH_COLOUR);  // no bg: keep the tile gradient
  String l1(label), l2;
  int font = 4;                         // big when it fits on one line
  if (tft.textWidth(l1, font) > maxW) font = 2;
  if (tft.textWidth(l1, font) > maxW) {
    int sp = l1.lastIndexOf(' ');
    while (sp > 0 && tft.textWidth(l1.substring(0, sp), font) > maxW) sp = l1.lastIndexOf(' ', sp - 1);
    if (sp > 0) { l2 = l1.substring(sp + 1); l1 = l1.substring(0, sp); }
    while (l1.length() > 1 && tft.textWidth(l1, font) > maxW) l1.remove(l1.length() - 1);
    while (l2.length() > 1 && tft.textWidth(l2, font) > maxW) l2.remove(l2.length() - 1);
  }
  int cx = ix + ICON_W / 2, cy = iy + ICON_H / 2;
  if (l2.length()) { tft.drawString(l1, cx, cy - 8, font); tft.drawString(l2, cx, cy + 9, font); }
  else tft.drawString(l1, cx, cy, font);
  if (connectedDot) tft.fillCircle(ix + ICON_W - 10, iy + 10, 2, TFT_GREEN);
}

// Label for a tile drawn as a blank key: input tiles use the TV's own name.
static void drawTileLabel(const TileDef& t, const char* fallback, int ix, int iy) {
  if (t.kind == TileKind::Input && !t.label[0]) {
    InputInfo in;
    char label[24];
    bool conn = false;
    if (lg.getInput(t.arg, in)) { strlcpy(label, in.label, sizeof(label)); conn = in.connected; }
    else if (!strncmp(t.arg, "HDMI_", 5)) snprintf(label, sizeof(label), "HDMI %s", t.arg + 5);
    else strlcpy(label, t.arg, sizeof(label));
    drawKeyLabel(label, ix, iy, conn);
    return;
  }
  drawKeyLabel(fallback, ix, iy, false);
}

static void tileRect(int idx, int& x, int& y) {
  x = (idx % COLS) * CELL_W;
  y = (idx / COLS) * CELL_H;
}

static void drawTile(int idx, bool highlight) {
  const TileDef& t = tileAt(idx);
  int x, y;
  tileRect(idx, x, y);
  tft.fillRect(x, y, CELL_W, CELL_H, BG);
  if (t.kind == TileKind::None) return;
  const char* label = nullptr;
  const uint16_t* icon = layout::tileIcon(t, dynOn(t), &label);
  int ix = x + (CELL_W - ICON_W) / 2, iy = y + (CELL_H - ICON_H) / 2;
  tft.pushImage(ix, iy, ICON_W, ICON_H, icon ? icon : ic_key_blank);
  if (!icon) drawTileLabel(t, label, ix, iy);
  if (highlight) {
    bool usable = lg.link.load() == LinkState::Registered || t.kind == TileKind::Power;
    uint16_t c = usable ? TFT_WHITE : TFT_RED;
    tft.drawRoundRect(ix - 3, iy - 3, ICON_W + 6, ICON_H + 6, 8, c);
    tft.drawRoundRect(ix - 4, iy - 4, ICON_W + 8, ICON_H + 8, 9, c);
  }
}

static void drawGrid() {
  for (int i = 0; i < COLS * ROWS; i++) drawTile(i, i == pressedTile);
}

// ------------------------------------------------------------ TV list

// Rows: stored TVs, then the scan button, then discovered TVs not yet stored.
struct ListRow { enum { Stored, Scan, Found } kind; int idx; };
static ListRow rows[COLS * ROWS];
static int rowCount = 0;

static void buildRows() {
  rowCount = 0;
  int maxRows = GRID_H / ROW_H - 1;  // 6 rows, last one is the Wi-Fi row
  int n = store.count();
  for (int i = 0; i < n && rowCount < maxRows - 1; i++) rows[rowCount++] = {ListRow::Stored, i};
  rows[rowCount++] = {ListRow::Scan, 0};
  int f = lg.foundCount();
  for (int i = 0; i < f && rowCount < maxRows; i++) {
    FoundTV ft;
    if (!lg.getFound(i, ft)) continue;
    if (store.findByIp(ft.ip) >= 0) continue;
    rows[rowCount++] = {ListRow::Found, i};
  }
}

static void drawListRow(int r) {
  int y = r * ROW_H;
  tft.fillRect(0, y, tft.width(), ROW_H, BG);
  const ListRow& row = rows[r];
  if (row.kind == ListRow::Stored) {
    TVRecord rec;
    if (!store.get(row.idx, rec)) return;
    bool sel = row.idx == store.selected();
    if (sel) tft.fillRoundRect(2, y + 2, tft.width() - 4, ROW_H - 4, 6, TILE_BG);
    tft.fillCircle(14, y + ROW_H / 2, 4, sel ? linkColour(lg.link.load()) : TFT_DARKGREY);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, sel ? TILE_BG : BG);
    tft.drawString(rec.name, 26, y + ROW_H / 2, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, sel ? TILE_BG : BG);
    tft.drawString(rec.clientKey[0] ? rec.ip : "not paired", 268, y + ROW_H / 2, 1);
    bool confirm = confirmForget == r && millis() < confirmUntil;
    textButton(280, y + 5, 36, ROW_H - 10, confirm ? "sure?" : "x", confirm ? TFT_RED : 0x4208, TFT_WHITE, 1);
  } else if (row.kind == ListRow::Scan) {
    bool busy = lg.scanning.load();
    textButton(6, y + 4, tft.width() - 12, ROW_H - 8, busy ? "Scanning..." : "Scan for TVs", busy ? 0x4208 : ACCENT, TFT_WHITE);
  } else {
    FoundTV ft;
    if (!lg.getFound(row.idx, ft)) return;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(GREEN, BG);
    tft.drawString("+", 12, y + ROW_H / 2, 4);
    tft.setTextColor(TFT_WHITE, BG);
    tft.drawString(ft.name, 30, y + ROW_H / 2, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString(ft.ip, tft.width() - 8, y + ROW_H / 2, 1);
  }
}

static void drawWifiRow() {
  int y = GRID_H - ROW_H;
  tft.fillRect(0, y, tft.width(), ROW_H, BG);
  tft.drawFastHLine(6, y, tft.width() - 12, 0x2124);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BG);
  char t[48];
  bool up = WiFi.status() == WL_CONNECTED;
  snprintf(t, sizeof(t), "Wi-Fi: %s", creds.valid() ? creds.ssid : "not set");
  tft.drawString(t, 12, y + ROW_H / 2, 2);
  tft.fillCircle(tft.width() - 100, y + ROW_H / 2, 3, up ? TFT_GREEN : (creds.valid() ? TFT_YELLOW : TFT_DARKGREY));
  textButton(tft.width() - 90, y + 5, 84, ROW_H - 10, "Change", 0x4208, TFT_WHITE);
}

static void drawList() {
  buildRows();
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  for (int r = 0; r < rowCount; r++) drawListRow(r);
  drawWifiRow();
  if (store.count() == 0 && lg.foundCount() == 0 && !lg.scanning.load()) {
    // Just below the last row (the Scan button), clear of the Wi-Fi row.
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString("No TVs yet.", tft.width() / 2, rowCount * ROW_H + 20, 2);
    tft.drawString("Turn the TV on, then tap \"Scan for TVs\".", tft.width() / 2, rowCount * ROW_H + 40, 2);
  }
}

// ------------------------------------------------------------- keypad

static const int KP_X = 140, KP_W = 60, KP_H = 54;
static const char* KP_KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK"};

static void drawKeypad() {
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  // Left panel
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BG);
  tft.drawString("Pair with", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, BG);
  tft.drawString(pairName, 8, 26, 2);
  tft.setTextColor(TFT_DARKGREY, BG);
  tft.drawString(pairIp, 8, 44, 1);
  // PIN box
  tft.fillRoundRect(8, 66, KP_X - 16, 34, 6, TILE_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TILE_BG);
  tft.drawString(pin[0] ? pin : "", 8 + (KP_X - 16) / 2, 83, 4);
  // Status
  const char* st = pkState == PK_Connecting ? "Connecting..."
                 : pkState == PK_Enter      ? "Enter the PIN on the TV"
                 : pkState == PK_Checking   ? "Checking..."
                 : pkState == PK_Wrong      ? "Pairing failed. Tap OK"
                                            : "Paired!";
  uint16_t sc = pkState == PK_Wrong ? TFT_RED : pkState == PK_Done ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRect(0, 108, KP_X, 50, BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(sc, BG);
  // wrap by hand: two short lines max
  String s(st);
  int sp = s.length() > 16 ? s.lastIndexOf(' ', 16) : -1;
  if (sp > 0) { tft.drawString(s.substring(0, sp), 8, 110, 2); tft.drawString(s.substring(sp + 1), 8, 128, 2); }
  else tft.drawString(s, 8, 110, 2);
  textButton(8, GRID_H - 44, KP_X - 16, 36, "Cancel", 0x4208, TFT_WHITE);
  // Keys
  for (int i = 0; i < 12; i++) {
    int x = KP_X + (i % 3) * KP_W, y = (i / 3) * KP_H;
    bool ok = i == 11;
    textButton(x + 3, y + 3, KP_W - 6, KP_H - 6, KP_KEYS[i], ok ? ACCENT : TILE_BG, TFT_WHITE, 4);
  }
}

static void drawPinOnly() {
  tft.fillRoundRect(8, 66, KP_X - 16, 34, 6, TILE_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TILE_BG);
  tft.drawString(pin[0] ? pin : "", 8 + (KP_X - 16) / 2, 83, 4);
}

// ----------------------------------------------------------- Wi-Fi list

static void startWifiScan() {
  wifiScanShown = false;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async; results picked up in syncState
}

static void drawWifiList() {
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  int n = WiFi.scanComplete();
  wifiRowCount = 0;
  if (n < 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString("Scanning for networks...", tft.width() / 2, GRID_H / 2 - 20, 2);
    return;
  }
  // strongest first, unique SSIDs
  int order[32]; int cnt = min(n, 32);
  for (int i = 0; i < cnt; i++) order[i] = i;
  for (int i = 0; i < cnt; i++) for (int j = i + 1; j < cnt; j++)
    if (WiFi.RSSI(order[j]) > WiFi.RSSI(order[i])) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  int shown = 0;
  String seen[5];
  for (int k = 0; k < cnt && shown < 5; k++) {
    int i = order[k];
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int q = 0; q < shown; q++) if (seen[q] == ssid) dup = true;
    if (dup) continue;
    seen[shown] = ssid;
    int y = shown * ROW_H;
    bool cur = creds.valid() && ssid == creds.ssid;
    if (cur) tft.fillRoundRect(2, y + 2, tft.width() - 4, ROW_H - 4, 6, TILE_BG);
    // signal bars
    int rssi = WiFi.RSSI(i);
    int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int b = 0; b < 4; b++) tft.fillRect(10 + b * 5, y + ROW_H / 2 + 6 - (b + 1) * 3, 3, (b + 1) * 3, b < bars ? TFT_GREEN : 0x4208);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, cur ? TILE_BG : BG);
    tft.drawString(ssid, 36, y + ROW_H / 2, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, cur ? TILE_BG : BG);
    tft.drawString(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "locked", tft.width() - 10, y + ROW_H / 2, 1);
    shown++;
  }
  wifiRowCount = shown;
  if (!shown) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString("No networks found", tft.width() / 2, 40, 2);
  }
  textButton(6, GRID_H - ROW_H + 4, tft.width() - 12, ROW_H - 8, "Rescan", ACCENT, TFT_WHITE);
}

// -------------------------------------------------------------- keyboard

static const int KB_Y = 40, KB_ROW_H = 44, KB_KEY_W = 32;
static const char* KB_ROWS_LOWER[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char* KB_ROWS_SYM[3]   = {"1234567890", "-_/:;()&@\"", "#%^*+=.,?!"};

static void kbRowGeom(int r, const char*& keys, int& x0) {
  keys = kbSym ? KB_ROWS_SYM[r] : KB_ROWS_LOWER[r];
  int n = strlen(keys);
  x0 = (tft.width() - n * KB_KEY_W) / 2;
  if (r == 2 && !kbSym) x0 = (tft.width() - 9 * KB_KEY_W) / 2 + KB_KEY_W;  // room for shift/backspace
}

static void drawKbField() {
  tft.fillRect(0, 0, tft.width(), KB_Y, BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BG);
  tft.drawString(kbSsid, 8, 10, 1);
  tft.fillRoundRect(6, 18, tft.width() - 12, 20, 4, TILE_BG);
  tft.setTextColor(TFT_WHITE, TILE_BG);
  String shown(kbText);
  while (shown.length() && tft.textWidth(shown, 2) > tft.width() - 24) shown.remove(0, 1);
  tft.drawString(shown + "_", 12, 28, 2);
}

static void drawKeyboard() {
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  drawKbField();
  char lbl[2] = {0, 0};
  for (int r = 0; r < 3; r++) {
    const char* keys; int x0;
    kbRowGeom(r, keys, x0);
    int y = KB_Y + r * KB_ROW_H;
    for (int i = 0; keys[i]; i++) {
      lbl[0] = kbShift && !kbSym ? toupper(keys[i]) : keys[i];
      textButton(x0 + i * KB_KEY_W + 2, y + 2, KB_KEY_W - 4, KB_ROW_H - 4, lbl, TILE_BG, TFT_WHITE, 2);
    }
  }
  int y2 = KB_Y + 2 * KB_ROW_H;
  if (!kbSym) {
    textButton(2, y2 + 2, 44, KB_ROW_H - 4, "shift", kbShift ? ACCENT : 0x4208, TFT_WHITE, 1);
  }
  textButton(tft.width() - 46, y2 + 2, 44, KB_ROW_H - 4, "<", 0x4208, TFT_WHITE, 2);
  int y3 = KB_Y + 3 * KB_ROW_H;
  textButton(2, y3 + 2, 60, KB_ROW_H - 4, kbSym ? "abc" : "?123", 0x4208, TFT_WHITE, 2);
  textButton(66, y3 + 2, tft.width() - 66 - 72, KB_ROW_H - 4, "space", TILE_BG, TFT_LIGHTGREY, 2);
  textButton(tft.width() - 70, y3 + 2, 68, KB_ROW_H - 4, "Join", ACCENT, TFT_WHITE, 2);
}

static void kbAppend(char c) {
  size_t n = strlen(kbText);
  if (n < sizeof(kbText) - 1) { kbText[n] = c; kbText[n + 1] = 0; }
  if (kbShift) { kbShift = false; drawKeyboard(); return; }
  drawKbField();
}

static void wifiApply() {
  strlcpy(creds.ssid, kbSsid, sizeof(creds.ssid));
  strlcpy(creds.pass, kbText, sizeof(creds.pass));
  wifiSave(creds.ssid, creds.pass);
  LOGF("[cyd] wifi credentials saved for %s\n", creds.ssid);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(creds.ssid, creds.pass);
  wifiStartedAt = millis();
}

// ----------------------------------------------------------------- bar

static void drawBar() {
  tft.fillRect(0, BAR_Y, tft.width(), BAR_H, BAR_BG);
  LinkState ls = lg.link.load();
  uint16_t dot = linkColour(ls);
  if (millis() < errorFlashUntil) dot = TFT_RED;
  tft.fillCircle(10, BAR_Y + BAR_H / 2, 4, dot);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, BAR_BG);
  char left[40];
  if (mode == Mode::TVList) snprintf(left, sizeof(left), "TVs");
  else if (mode == Mode::Keypad) snprintf(left, sizeof(left), "Pairing");
  else if (mode == Mode::WifiList) snprintf(left, sizeof(left), "Choose a network");
  else if (mode == Mode::Keyboard) snprintf(left, sizeof(left), "Password");
  else if (ls == LinkState::NoWifi) {
    if (!creds.valid()) snprintf(left, sizeof(left), "No Wi-Fi - tap to set up");
    else if (WiFi.status() == WL_CONNECTED) snprintf(left, sizeof(left), "wifi ok");
    else if (millis() - wifiStartedAt > 30000) snprintf(left, sizeof(left), "Wi-Fi failed - tap");
    else snprintf(left, sizeof(left), "wifi...");
  }
  else {
    TVRecord rec;
    if (store.get(store.selected(), rec)) snprintf(left, sizeof(left), "%s", rec.name);
    else snprintf(left, sizeof(left), "No TV - tap to add");
  }
  tft.drawString(left, 20, BAR_Y + BAR_H / 2, 2);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BAR_BG);
  char right[32];
  if (mode == Mode::Grid) snprintf(right, sizeof(right), "%s  %d/%d >", layout::current.pages[page].name, page + 1, PAGE_COUNT);
  else snprintf(right, sizeof(right), mode == Mode::TVList ? "Back" : "Cancel");
  tft.drawString(right, tft.width() - 6, BAR_Y + BAR_H / 2, 2);
}

static void setMode(Mode m) {
  mode = m;
  pressedTile = -1;
  confirmForget = -1;
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  if (mode == Mode::Grid) drawGrid();
  else if (mode == Mode::TVList) drawList();
  else if (mode == Mode::Keypad) drawKeypad();
  else if (mode == Mode::WifiList) { startWifiScan(); drawWifiList(); }
  else drawKeyboard();
  drawBar();
}

// --------------------------------------------------------------- input

static void fire(const TileDef& t) {
  char payload[64];
  switch (t.kind) {
    case TileKind::Power:      lg.post(CmdType::PowerToggle); break;
    case TileKind::Button:     lg.post(CmdType::Button, t.arg); break;
    case TileKind::Ssap:       lg.post(CmdType::Request, t.arg, t.payload); break;
    case TileKind::App:        lg.post(CmdType::LaunchApp, t.arg[0] ? t.arg : t.icon); break;
    case TileKind::Input:
      snprintf(payload, sizeof(payload), "{\"inputId\":\"%s\"}", t.arg);
      lg.post(CmdType::Request, "ssap://tv/switchInput", payload);
      break;
    case TileKind::Mute:       lg.post(CmdType::MuteToggle); break;
    case TileKind::PlayPause:  lg.post(CmdType::PlayPause); break;
    case TileKind::Screen:     lg.post(CmdType::ScreenToggle); break;
    case TileKind::VolumeUp:   lg.post(CmdType::Request, "ssap://audio/volumeUp"); break;
    case TileKind::VolumeDown: lg.post(CmdType::Request, "ssap://audio/volumeDown"); break;
    default: break;
  }
}

static void startPairing(const char* ip, const char* name) {
  strlcpy(pairIp, ip, sizeof(pairIp));
  strlcpy(pairName, name, sizeof(pairName));
  pin[0] = 0;
  pkState = PK_Connecting;
  lg.post(CmdType::PairStart, ip, name);
  setMode(Mode::Keypad);
}

static void onPressGrid(int x, int y) {
  if (y >= BAR_Y) {
    if (x < tft.width() / 2) setMode(Mode::TVList);
    else { page = (page + 1) % PAGE_COUNT; drawGrid(); drawBar(); }
    return;
  }
  int idx = (y / CELL_H) * COLS + (x / CELL_W);
  if (idx < 0 || idx >= COLS * ROWS) return;
  const TileDef& t = tileAt(idx);
  if (t.kind == TileKind::None) return;
  pressedTile = idx;
  pressStart = lastRepeat = millis();
  drawTile(idx, true);
  fire(t);
}

static void onPressList(int x, int y) {
  if (y >= BAR_Y) { setMode(Mode::Grid); return; }
  int r = y / ROW_H;
  if (r == GRID_H / ROW_H - 1) { setMode(Mode::WifiList); return; }   // Wi-Fi row
  if (r >= rowCount) return;
  const ListRow& row = rows[r];
  char idx[4];
  if (row.kind == ListRow::Stored) {
    if (x >= 280) {
      if (confirmForget == r && millis() < confirmUntil) {
        snprintf(idx, sizeof(idx), "%d", row.idx);
        lg.post(CmdType::Forget, idx);
        confirmForget = -1;
      } else {
        confirmForget = r;
        confirmUntil = millis() + 3000;
        drawListRow(r);
      }
      return;
    }
    TVRecord rec;
    if (!store.get(row.idx, rec)) return;
    if (!rec.clientKey[0]) { startPairing(rec.ip, rec.name); return; }
    snprintf(idx, sizeof(idx), "%d", row.idx);
    lg.post(CmdType::SelectTV, idx);
    setMode(Mode::Grid);
  } else if (row.kind == ListRow::Scan) {
    if (!lg.scanning.load()) { lg.post(CmdType::Scan); drawListRow(r); }
  } else {
    FoundTV ft;
    if (lg.getFound(row.idx, ft)) startPairing(ft.ip, ft.name);
  }
}

static void onPressKeypad(int x, int y) {
  if (y >= BAR_Y || (x < KP_X && y >= GRID_H - 44)) {  // bar or Cancel
    lg.post(CmdType::CancelPair);
    setMode(Mode::TVList);
    return;
  }
  if (x < KP_X) return;
  int i = (y / KP_H) * 3 + (x - KP_X) / KP_W;
  if (i < 0 || i > 11) return;
  if (pkState == PK_Done) return;
  if (pkState == PK_Wrong) {
    if (i == 11) {
      pin[0] = 0;
      lg.pairFailed = false;
      pkState = PK_Connecting;
      lg.post(CmdType::PairStart, pairIp, pairName);
      drawKeypad();
    }
    return;
  }
  if (pkState != PK_Enter) return;
  if (i == 9) {                       // backspace
    size_t n = strlen(pin);
    if (n) pin[n - 1] = 0;
    drawPinOnly();
  } else if (i == 11) {               // OK
    if (strlen(pin) != 8) return;
    if (pkState != PK_Enter && pkState != PK_Wrong) return;
    pkState = PK_Checking;
    lg.post(CmdType::SubmitPin, pin);
    drawKeypad();
  } else {
    size_t n = strlen(pin);
    if (n < 8) { pin[n] = KP_KEYS[i][0]; pin[n + 1] = 0; drawPinOnly(); }
  }
}

static void onPressWifiList(int x, int y) {
  if (y >= BAR_Y) { setMode(store.count() || creds.valid() ? Mode::TVList : Mode::Grid); return; }
  int r = y / ROW_H;
  if (r == GRID_H / ROW_H - 1) { startWifiScan(); drawWifiList(); return; }   // Rescan
  if (r >= wifiRowCount || WiFi.scanComplete() < 0) return;
  // Recover the SSID for row r the same way drawWifiList ordered them.
  int n = WiFi.scanComplete();
  int order[32]; int cnt = min(n, 32);
  for (int i = 0; i < cnt; i++) order[i] = i;
  for (int i = 0; i < cnt; i++) for (int j = i + 1; j < cnt; j++)
    if (WiFi.RSSI(order[j]) > WiFi.RSSI(order[i])) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  int shown = 0; String seen[5]; int pick = -1;
  for (int k = 0; k < cnt && shown < 5; k++) {
    String ssid = WiFi.SSID(order[k]);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int q = 0; q < shown; q++) if (seen[q] == ssid) dup = true;
    if (dup) continue;
    seen[shown] = ssid;
    if (shown == r) { pick = order[k]; break; }
    shown++;
  }
  if (pick < 0) return;
  strlcpy(kbSsid, WiFi.SSID(pick).c_str(), sizeof(kbSsid));
  kbText[0] = 0; kbShift = false; kbSym = false;
  if (WiFi.encryptionType(pick) == WIFI_AUTH_OPEN) { wifiApply(); setMode(Mode::Grid); return; }
  setMode(Mode::Keyboard);
}

static void onPressKeyboard(int x, int y) {
  if (y >= BAR_Y) { setMode(Mode::WifiList); return; }
  if (y < KB_Y) return;
  int r = (y - KB_Y) / KB_ROW_H;
  if (r == 3) {
    if (x < 64) { kbSym = !kbSym; kbShift = false; drawKeyboard(); }
    else if (x >= tft.width() - 72) { if (kbText[0]) { wifiApply(); setMode(Mode::Grid); } }
    else kbAppend(' ');
    return;
  }
  if (r == 2) {
    if (x >= tft.width() - 48) {                       // backspace
      size_t n = strlen(kbText);
      if (n) kbText[n - 1] = 0;
      drawKbField();
      return;
    }
    if (!kbSym && x < 48) { kbShift = !kbShift; drawKeyboard(); return; }
  }
  const char* keys; int x0;
  kbRowGeom(r, keys, x0);
  int i = (x - x0) / KB_KEY_W;
  if (x < x0 || i < 0 || i >= (int)strlen(keys)) return;
  char c = keys[i];
  if (kbShift && !kbSym) c = toupper(c);
  kbAppend(c);
}

static void onPress(int x, int y) {
  switch (mode) {
    case Mode::Grid:     onPressGrid(x, y); break;
    case Mode::TVList:   onPressList(x, y); break;
    case Mode::Keypad:   onPressKeypad(x, y); break;
    case Mode::WifiList: onPressWifiList(x, y); break;
    case Mode::Keyboard: onPressKeyboard(x, y); break;
  }
}

static void onHold() {
  if (mode != Mode::Grid || pressedTile < 0) return;
  const TileDef& t = tileAt(pressedTile);
  if (!t.repeat) return;
  uint32_t now = millis();
  if (now - pressStart > 450 && now - lastRepeat > 180) {
    lastRepeat = now;
    fire(t);
  }
}

static void onRelease() {
  if (mode == Mode::Grid && pressedTile >= 0) {
    int idx = pressedTile;
    pressedTile = -1;
    drawTile(idx, false);
  }
}

static void pollTouch() {
  static bool wasDown = false;
  bool down = touch.touched();
  if (down && !wasDown) {
    TS_Point p = touch.getPoint();
    // Ignore phantom presses: the IRQ line can twitch (notably at boot) with
    // no real pressure, in which case the controller reports z = 0.
    if (p.z < 150 || millis() < 1500) return;
    lastTouchAt = millis();
    if (dimmed) { dimmed = false; ledcWrite(BL_CH, 255); wasDown = true; return; }  // wake only
    int x = constrain((int)map(p.x, RAW_MIN, RAW_MAX, 0, tft.width()), 0, tft.width() - 1);
    int y = constrain((int)map(p.y, RAW_MIN, RAW_MAX, 0, tft.height()), 0, tft.height() - 1);
    LOGF("[touch] %d,%d raw=%d,%d z=%d\n", x, y, p.x, p.y, p.z);
    onPress(x, y);
  } else if (down && wasDown) {
    onHold();
  } else if (!down && wasDown) {
    onRelease();
  }
  wasDown = down;
}

// --------------------------------------------------------- serial debug
//
//   S          dump the framebuffer: "SCREEN w h\n" then h rows of w*3 raw
//              RGB bytes (see tools/cyd.py)
//   t X Y      simulate a tap at screen coords
//   p N        jump to page N (0-based)
//   b NAME     send a remote button (UP, ENTER, HOME...) via the pointer socket
//   v          open the TV list       scan     start discovery
//   pair IP    start pairing with IP  pin N    submit PIN
//   h          free heap

static void dumpScreen() {
  static uint8_t row[320 * 3];
  xSemaphoreTake(g_serialMutex, portMAX_DELAY);  // nobody else may print mid-dump
  Serial.printf("SCREEN %d %d\n", tft.width(), tft.height());
  Serial.flush();
  for (int y = 0; y < tft.height(); y++) {
    tft.readRectRGB(0, y, tft.width(), 1, row);
    Serial.write(row, tft.width() * 3);
  }
  Serial.flush();
  Serial.println();
  Serial.println("SCREEN_END");
  xSemaphoreGive(g_serialMutex);
}

static void handleSerial() {
  static char line[48];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;
    if (!line[0]) continue;
    if (!strcmp(line, "scan")) { lg.post(CmdType::Scan); continue; }
    if (!strcmp(line, "layout reset")) { layout::resetDefault(); continue; }
    if (!strncmp(line, "wifi ", 5)) {   // wifi SSID PASSWORD  (SSID may not contain spaces here)
      char* sp = strchr(line + 5, ' ');
      if (sp) { *sp = 0; strlcpy(kbSsid, line + 5, sizeof(kbSsid)); strlcpy(kbText, sp + 1, sizeof(kbText)); wifiApply(); }
      continue;
    }
    if (!strcmp(line, "w")) { setMode(mode == Mode::WifiList ? Mode::Grid : Mode::WifiList); continue; }
    if (!strcmp(line, "k")) { strlcpy(kbSsid, "Demo Network", sizeof(kbSsid)); kbText[0] = 0; setMode(Mode::Keyboard); continue; }
    if (!strncmp(line, "pair ", 5)) { startPairing(line + 5, "LG TV"); continue; }
    if (!strncmp(line, "pin ", 4)) { strlcpy(pin, line + 4, sizeof(pin)); pkState = PK_Checking; lg.post(CmdType::SubmitPin, pin); if (mode == Mode::Keypad) drawKeypad(); continue; }
    char* arg = line + 1;
    while (*arg == ' ') arg++;
    switch (line[0]) {
      case 'S': dumpScreen(); break;
      case 'h': LOGF("[dbg] heap %u\n", ESP.getFreeHeap()); break;
      case 'v': setMode(mode == Mode::TVList ? Mode::Grid : Mode::TVList); break;
      case 'p': page = constrain(atoi(arg), 0, PAGE_COUNT - 1); if (mode != Mode::Grid) setMode(Mode::Grid); else { drawGrid(); drawBar(); } LOGF("[dbg] page %d\n", page); break;
      case 'b': lg.post(CmdType::Button, arg); break;
      case 't': {
        int x = 0, y = 0;
        if (sscanf(arg, "%d %d", &x, &y) == 2) {
          LOGF("[dbg] tap %d,%d\n", x, y);
          onPress(x, y);
          delay(120);
          onRelease();
        }
        break;
      }
      default: LOGF("[dbg] unknown '%s'\n", line); break;
    }
  }
}

// ---------------------------------------------------------- state sync

static void syncState() {
  bool barDirty = false, gridDirty = false, listDirty = false, keypadDirty = false;

  LinkState ls = lg.link.load();
  if (ls != shownLink) { shownLink = ls; barDirty = true; listDirty = true; keypadDirty = true; }
  int tv = store.selected();
  if (tv != shownTV) { shownTV = tv; barDirty = true; listDirty = true; }
  uint32_t sg = store.generation();
  if (sg != shownStoreGen) { shownStoreGen = sg; barDirty = true; listDirty = true; }
  uint32_t fg = lg.foundGen.load();
  if (fg != shownFoundGen) { shownFoundGen = fg; listDirty = true; }
  static bool shownScanning = false;
  if (lg.scanning.load() != shownScanning) { shownScanning = lg.scanning.load(); listDirty = true; }

  bool m = lg.muted.load(), p = lg.playing.load(), s = lg.screenOff.load();
  if (m != shownMuted || p != shownPlaying || s != shownScreenOff) {
    shownMuted = m; shownPlaying = p; shownScreenOff = s;
    gridDirty = true;
  }
  static uint32_t shownInputsGen = 0;
  if (lg.inputsGen.load() != shownInputsGen) { shownInputsGen = lg.inputsGen.load(); gridDirty = true; }

  uint32_t err = lg.lastError.load();
  if (err != shownError) { shownError = err; errorFlashUntil = millis() + 600; barDirty = true; }
  static bool flashing = false;
  bool nowFlashing = millis() < errorFlashUntil;
  if (nowFlashing != flashing) { flashing = nowFlashing; barDirty = true; }

  static wl_status_t shownWifi = WL_IDLE_STATUS;
  if (WiFi.status() != shownWifi) { shownWifi = WiFi.status(); barDirty = true; listDirty = true; }
  static bool shownWifiFail = false;
  bool wifiFail = creds.valid() && WiFi.status() != WL_CONNECTED && millis() - wifiStartedAt > 30000;
  if (wifiFail != shownWifiFail) { shownWifiFail = wifiFail; barDirty = true; }

  if (mode == Mode::WifiList && !wifiScanShown && WiFi.scanComplete() >= 0) { wifiScanShown = true; drawWifiList(); }

  if (!dimmed && millis() - lastTouchAt > DIM_AFTER_MS) { dimmed = true; ledcWrite(BL_CH, 24); }

  // Forget confirmation timeout
  if (confirmForget >= 0 && millis() >= confirmUntil) { confirmForget = -1; listDirty = true; }

  // Pairing state machine (driven by the net task's atomics)
  if (mode == Mode::Keypad) {
    auto prev = pkState;
    if (lg.pairFailed.load() && pkState != PK_Wrong) { pkState = PK_Wrong; pin[0] = 0; }
    else if (ls == LinkState::NeedsPin && (pkState == PK_Connecting || (pkState == PK_Wrong && !lg.pairFailed.load()))) pkState = PK_Enter;
    else if (ls == LinkState::Registered && !lg.pairing.load() && pkState != PK_Done) { pkState = PK_Done; pkDoneAt = millis(); }
    if (pkState != prev) keypadDirty = true;
    if (pkState == PK_Done && millis() - pkDoneAt > 1200) { setMode(Mode::Grid); return; }
  }

  static uint32_t shownLayoutGen = 0;
  if (layout::generation != shownLayoutGen) {
    shownLayoutGen = layout::generation;
    if (page >= PAGE_COUNT) page = 0;
    if (mode == Mode::Grid) { drawGrid(); barDirty = true; }
    gridDirty = false;
  } else if (mode == Mode::Grid && gridDirty) {
    for (int i = 0; i < COLS * ROWS; i++)
      if (tileIsDynamic(tileAt(i))) drawTile(i, i == pressedTile);
  }
  if (mode == Mode::TVList && listDirty) drawList();
  if (mode == Mode::Keypad && keypadDirty) drawKeypad();
  if (barDirty) drawBar();
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(460800);
  g_serialMutex = xSemaphoreCreateMutex();
  delay(100);
  Serial.println("\n[cyd] LG remote booting");

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(BG);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  store.begin();
  layout::begin();
  drawGrid();
  drawBar();

  // Backlight on PWM so it can dim when idle.
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(TFT_BL, BL_CH);
  ledcWrite(BL_CH, 255);
  lastTouchAt = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  creds = wifiLoad();
  if (creds.valid()) {
    WiFi.begin(creds.ssid, creds.pass);
    wifiStartedAt = millis();
    LOGF("[cyd] wifi connecting to %s\n", creds.ssid);
  } else {
    LOGF("[cyd] no wifi credentials; opening setup\n");
    setMode(Mode::WifiList);
  }

  lg.begin(&store);
  webui::begin(&tft, &lg, &store);
  LOGF("[cyd] %d TVs, selected %d\n", store.count(), store.selected());
}

void loop() {
  static bool wifiLogged = false;
  if (!wifiLogged && WiFi.status() == WL_CONNECTED) {
    wifiLogged = true;
    LOGF("[cyd] wifi up, ip %s\n", WiFi.localIP().toString().c_str());
  }
  pollTouch();
  handleSerial();
  webui::loop();
  syncState();
  delay(10);
}
