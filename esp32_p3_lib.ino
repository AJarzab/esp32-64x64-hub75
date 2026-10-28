// ============================================================
//  ESP32 + HUB75 64x64 LED Matrix Web Player v3
//  - upload GIF/JPG/PNG/WebP/BMP over the web
//  - browser-side auto-conversion of images to 64x64
//  - GIF loops until changed from the UI (no flicker)
//  - brightness, delete, file list, status
//  - animated 64x64 preview with live delay control
//  - drag-to-crop on source, applied to preview and upload
//  v3 playback pack (ESP32 WROOM, no PSRAM):
//  - playlist auto-rotate + shuffle, pause/resume, next/prev
//  - settings and last file persist in NVS (survive reboot)
//  - Wi-Fi AP fallback + "change Wi-Fi" form in the web UI
//  - live stats: FPS, heap, RSSI, flash usage, uptime
//  - upload pre-flight free-space check
//  v3.2 P3 Pro:
//  - tear-free double buffering + fade transitions between files
//  - power limiter, gamma-friendly colors
//  - NTP clock (digital + analog), on/off schedule with overnight windows
//  - playlist editor: order, per-file hold time, enable/disable
//  - multi-file upload queue, rename, delete all (no thumbnails: the browser
//    never downloads media files, the upload preview is local)
//  - WebSocket live status, optional PIN (cookie session), captive portal
//  - QR code on the panel at boot / on demand
//  - config export and settings reset
//  v3.4 (this build):
//  - text mode, ambient effects and thumbnail downloads removed; GIF playback,
//    images, playlist, clock and schedule stay
//  - full 8-bit colour depth (256 levels per channel)
//  - panel rotation 0/90/180/270, saved in NVS
//  - single framebuffer: a 64x64 double-buffered panel eats ~99 KB of internal
//    RAM on a board without PSRAM, which fragments the heap until
//    AsyncWebServer cannot allocate its 2.9 KB send buffer (the page returned
//    headers but no body). One buffer keeps ~40 KB contiguous.
//  - heap reservation released right before server.begin(), so AsyncTCP's
//    16 KB task stack always fits; listener is retried from loop() if needed
//  - network bring-up reordered: listener first, association/AP afterwards
//  - HTTP admission control: the UI page is sent one client at a time; extra
//    page requests are dropped without allocating a response
//  - guarded panel init (falls back to no panel)
//  - crash-loop guard: 3 fast restarts -> safe mode (no QR, no auto-resume)
//  - uploads that die mid-body are reaped after 20 s (LittleFS has only a few
//    descriptors), and a file still being written is never opened for playback
//  - white balance options removed; colours go straight to the panel
//  - watchdog: if a client's socket stops draining for 40 s the async task is
//    presumed wedged and the chip restarts instead of answering ping forever
//  - /diag endpoint with heap, reset reason and boot stage
// ============================================================

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <AnimatedGIF.h>
#include <JPEGDEC.h>
#include "qr_local.h"
#include <time.h>
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

// ==================== CONFIGURATION ====================
const char* SSID = "your-ssid";
const char* PASS = "your-password";

#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 32
#define B2_PIN 33
#define A_PIN  23
#define B_PIN  19
#define C_PIN  22
#define D_PIN  17
#define E_PIN  18
#define LAT_PIN 4
#define OE_PIN  13
#define CLK_PIN 16

#define PANEL_W 64
#define PANEL_H 64
#define MAX_UPLOAD_SIZE (1 * 1024 * 1024)
#define AP_PASSWORD "ledmatrix"
#define MIN_FREE_BYTES (64 * 1024)
#define DEFAULT_ROTATE_SEC 30
#define FW_VERSION "3.4"
#define GIF_CANVAS_PIXELS (PANEL_W * PANEL_H)

// ==================== GLOBALS ====================
MatrixPanel_I2S_DMA *dma_display = nullptr;
AsyncWebServer server(80);
AnimatedGIF gif;
JPEGDEC jpeg;
Preferences prefs;

File gifFile;
File jpegFile;

bool playingGif = false;
bool showingJpeg = false;
String currentFile = "";
uint32_t nextFrameTime = 0;
uint32_t frameCount = 0;
uint8_t currentBrightness = 90;

// --- playlist / playback ---
bool paused = false;
bool autoRotate = false;
bool shuffleMode = false;
uint16_t rotateSeconds = DEFAULT_ROTATE_SEC;
uint32_t playStartedAt = 0;      // millis() when the current file started
uint32_t pausedAccumMs = 0;      // paused time accumulated for this file
uint32_t pauseStartedAt = 0;

// --- runtime stats ---
uint32_t fpsFrames = 0;
uint32_t fpsWindowStart = 0;
uint16_t currentFps = 0;

// --- wifi ---
String wifiSsid = SSID;
String wifiPass = PASS;
bool apFallback = false;
String apSsid = "";
uint32_t lastWifiRetry = 0;
uint8_t wifiFailCount = 0;

// --- clock ---
bool showingClock = false;
uint8_t clockFace = 0;         // 0=digital 1=analog
bool clockDate = true;
String timeZone = "UTC0";
bool timeReady = false;
uint32_t lastClockTick = 0;

// --- on-demand QR screen ---
bool showingQr = false;
bool scanQrAtBoot = true;
uint32_t qrUntil = 0;
String qrSavedFile = "";

// --- schedule ---
bool schedEnabled = false;
uint16_t schedOnMin = 8 * 60;    // minutes since midnight
uint16_t schedOffMin = 23 * 60;
bool displayAsleep = false;
uint32_t lastSchedCheck = 0;

// --- color / power / transitions ---
uint8_t powerLimit = 100;      // percent of max brightness
bool fadeEnabled = true;
uint8_t panelBrightness = 90;  // actual value currently sent to the panel
bool displayOk = false;        // panel controller came up; false = drawing must be skipped
uint8_t panelRotation = 0;     // 0..3 = 0/90/180/270 degrees, applied by the driver transform

// --- boot diagnostics / crash-loop guard (kept in RTC RAM across reboots) ---
RTC_NOINIT_ATTR uint32_t bootMarker;   // magic when the previous boot reached the loop
RTC_NOINIT_ATTR uint32_t bootCount;    // consecutive fast restarts
RTC_NOINIT_ATTR uint32_t bootStage;    // last setup() milestone for diagnostics
#define BOOT_MARKER_MAGIC 0x4C4D7633UL   // "LMv3"
bool safeMode = false;                 // entered after repeated early restarts
uint32_t bootStartedAt = 0;
uint32_t minFreeHeap = 0xFFFFFFFFUL;
uint8_t  bootResetReason = 0;
esp_timer_handle_t bootGuardTimer = nullptr;   // reboots if setup() stalls

// --- heap reservation -------------------------------------------------------
// AsyncTCP creates its service task with a 16 KB contiguous stack. If boot-time
// fragmentation leaves no such block, begin() fails silently and nothing listens
// on port 80 (ping and mDNS still work). We hold a block from early boot and
// release it right before server.begin() so that allocation is guaranteed.
void *heapReserve = nullptr;
const size_t HEAP_RESERVE_BYTES = 20480;
bool serverListening = false;      // AsyncTCP task exists -> port 80 is open
uint32_t listenRetryAt = 0;
bool mdnsStarted = false;

// Single-flight guard for the big HTML page (see the "/" route). A second tab
// loading at the same time would double the in-flight TCP memory.
bool pageSending = false;
uint32_t pageSendStarted = 0;

// Upload stream state. Kept at file scope so loop() can reap a handle whose
// request died mid-body: LittleFS only has a handful of file descriptors, and
// leaking them makes every later open() fail.
File uploadFile;
String uploadPath;
String uploadError;
size_t uploadBytes = 0;
uint32_t uploadLastAt = 0;

// --- auth / network services ---
String adminPin = "";
String authToken = "";
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
uint32_t lastWsPush = 0;
uint32_t wsAuth[4] = {0, 0, 0, 0};   // client ids that presented the session token

// --- compositing ---
// The GIF decoder composes each frame into this canvas (64x64, RGB565) before
// it is pushed to the panel, so transparency and disposal methods are applied
// once per frame instead of writing partial rows directly to the display.
uint16_t gifCanvas[GIF_CANVAS_PIXELS];

// --- deferred work (done from loop, not from the async handler) ---
bool prefsDirty = false;
uint32_t prefsSaveAt = 0;
bool rebootPending = false;
uint32_t rebootAt = 0;

// Serializes playback/file access between loop() and the web-server task.
SemaphoreHandle_t playLock = nullptr;

void lockPlayback() {
  if (playLock) xSemaphoreTakeRecursive(playLock, portMAX_DELAY);
}
void unlockPlayback() {
  if (playLock) xSemaphoreGiveRecursive(playLock);
}

struct PlaybackLock {
  PlaybackLock()  { lockPlayback(); }
  ~PlaybackLock() { unlockPlayback(); }
};

// ==================== DISPLAY HELPERS ====================
uint8_t effectiveBrightness() {
  uint32_t b = (uint32_t)currentBrightness * powerLimit / 100;
  if (b < 1) b = 1;
  if (b > 255) b = 255;
  return (uint8_t)b;
}

void setPanelBrightness(uint8_t b) {
  panelBrightness = b;
  if (dma_display) dma_display->setBrightness8(b);
}

void applyBrightness() {
  setPanelBrightness(displayAsleep ? 0 : effectiveBrightness());
}

void presentFrame() {
  if (!dma_display || !displayOk) return;
  if (dma_display->getCfg().double_buff) dma_display->flipDMABuffer();
}

void clearPanel() {
  if (!dma_display) return;
  dma_display->clearScreen();
  presentFrame();
}

// ==================== GIF CALLBACKS ====================
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s = pDraw->pPixels;
  uint16_t *usPalette = pDraw->pPalette;
  int y = pDraw->iY + pDraw->y;
  if (y < 0 || y >= PANEL_H) return;

  if (pDraw->ucDisposalMethod == 2) {
    for (int x = 0; x < pDraw->iWidth; x++)
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    for (int x = 0; x < pDraw->iWidth; x++) {
      int px = pDraw->iX + x;
      if (px < 0 || px >= PANEL_W) continue;
      if (s[x] != pDraw->ucTransparent)
        gifCanvas[y * PANEL_W + px] = usPalette[s[x]];
    }
  } else {
    for (int x = 0; x < pDraw->iWidth; x++) {
      int px = pDraw->iX + x;
      if (px < 0 || px >= PANEL_W) continue;
      gifCanvas[y * PANEL_W + px] = usPalette[s[x]];
    }
  }
}

// Push the composed GIF canvas to the panel in one pass, then flip.
void pushGifCanvas() {
  if (!dma_display) return;
  for (int y = 0; y < PANEL_H; y++) {
    const uint16_t *row = &gifCanvas[y * PANEL_W];
    for (int x = 0; x < PANEL_W; x++)
      dma_display->drawPixel(x, y, row[x]);
  }
  presentFrame();
}

void *gifOpen(const char *fname, int32_t *size) {
  gifFile = LittleFS.open(fname, "r");
  if (!gifFile) return NULL;
  *size = gifFile.size();
  return &gifFile;
}
void gifCloseCB(void *h) {
  File *f = static_cast<File*>(h);
  if (f && *f) f->close();
}
int32_t gifRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = static_cast<File*>(pFile->fHandle);
  if (!f || !*f) return 0;
  f->seek(pFile->iPos);
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos += n;
  return n;
}
int32_t gifSeek(GIFFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File*>(pFile->fHandle);
  if (!f || !*f) return 0;
  f->seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}

// ==================== JPEG CALLBACKS ====================
void *jpegOpen(const char *fname, int32_t *size) {
  jpegFile = LittleFS.open(fname, "r");
  if (!jpegFile) return NULL;
  *size = jpegFile.size();
  return &jpegFile;
}
void jpegCloseCB(void *h) {
  File *f = static_cast<File*>(h);
  if (f && *f) f->close();
}
int32_t jpegRead(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = static_cast<File*>(pFile->fHandle);
  if (!f || !*f) return 0;
  f->seek(pFile->iPos);
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos += n;
  return n;
}
int32_t jpegSeek(JPEGFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File*>(pFile->fHandle);
  if (!f || !*f) return 0;
  f->seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}
int jpegDraw(JPEGDRAW *pDraw) {
  for (int y = 0; y < pDraw->iHeight; y++) {
    for (int x = 0; x < pDraw->iWidth; x++) {
      int px = pDraw->x + x, py = pDraw->y + y;
      if (px < 0 || px >= PANEL_W || py < 0 || py >= PANEL_H) continue;
      dma_display->drawPixel(px, py, pDraw->pPixels[y * pDraw->iWidth + x]);
    }
  }
  return 1;
}

// ==================== PLAYBACK CONTROL ====================
void stopPlayback(bool forget = false, bool clear = true) {
  PlaybackLock guard;
  if (playingGif) { gif.close(); playingGif = false; }
  if (showingJpeg) { jpeg.close(); showingJpeg = false; }
  showingClock = false;
  showingQr = false;
  currentFile = "";
  paused = false;
  pausedAccumMs = 0;
  playStartedAt = 0;
  if (clear) clearPanel();
  if (forget) prefs.putString("last", "");
}

bool restartGif(const String &path) {
  if (path.length() == 0) return false;
  if (playingGif) { gif.close(); playingGif = false; }
  memset(gifCanvas, 0, sizeof(gifCanvas));
  if (gif.open(path.c_str(), gifOpen, gifCloseCB, gifRead, gifSeek, GIFDraw)) {
    playingGif = true;
    frameCount = 0;
    nextFrameTime = millis();
    return true;
  }
  Serial.printf("[GIF] restart FAILED '%s'\n", path.c_str());
  return false;
}

bool isGifHeader(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t hdr[6];
  int n = f.read(hdr, 6);
  f.close();
  return n == 6 &&
    (memcmp(hdr, "GIF87a", 6) == 0 || memcmp(hdr, "GIF89a", 6) == 0);
}

// ==================== PLAYLIST ====================
bool playFile(String path);   // defined below
bool playWithFade(String path);
void restartNow();            // defined below

String stripSlash(String p) {
  while (p.startsWith("/")) p = p.substring(1);
  return p;
}

bool isMediaFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".gif") || lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

std::vector<String> scanFiles() {
  std::vector<String> out;
  File root = LittleFS.open("/");
  if (!root) return out;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = stripSlash(String(f.name()));
      if (name.length() && isMediaFile(name)) out.push_back(name);
    }
    f = root.openNextFile();
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---- playlist (order, enable, per-file hold time) ----
struct PlEntry { String name; uint16_t dur; bool en; };
std::vector<PlEntry> playlist;

int plIndex(const String &name) {
  for (size_t i = 0; i < playlist.size(); i++)
    if (playlist[i].name == name) return (int)i;
  return -1;
}

bool plHas(const std::vector<String> &v, const String &s) {
  for (auto &x : v) if (x == s) return true;
  return false;
}

// /playlist.txt, one entry per line: <enabled>|<seconds, 0 = global>|<name>
void loadPlaylist() {
  playlist.clear();
  File f = LittleFS.open("/playlist.txt", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    int a = line.indexOf('|');
    int b = line.indexOf('|', a + 1);
    if (a < 1 || b < 0) continue;
    PlEntry e;
    e.en = line.substring(0, a) == "1";
    e.dur = (uint16_t)constrain(line.substring(a + 1, b).toInt(), 0, 3600);
    e.name = line.substring(b + 1);
    if (e.name.length()) playlist.push_back(e);
  }
  f.close();
}

void savePlaylist() {
  File f = LittleFS.open("/playlist.txt", "w");
  if (!f) return;
  for (auto &e : playlist)
    f.printf("%d|%u|%s\n", e.en ? 1 : 0, (unsigned)e.dur, e.name.c_str());
  f.close();
}

// Keep the playlist in step with the filesystem: drop missing, append new.
void syncPlaylist() {
  PlaybackLock guard;
  std::vector<String> disk = scanFiles();
  bool changed = false;
  for (size_t i = playlist.size(); i-- > 0;)
    if (!plHas(disk, playlist[i].name)) { playlist.erase(playlist.begin() + i); changed = true; }
  for (auto &n : disk)
    if (plIndex(n) < 0) { playlist.push_back({n, 0, true}); changed = true; }
  if (changed) savePlaylist();
}

std::vector<String> listMediaFiles() {
  std::vector<String> out;
  for (auto &e : playlist) if (e.en) out.push_back(e.name);
  if (playlist.empty()) return scanFiles();   // no playlist yet: plain disk order
  return out;
}

int currentIndexIn(const std::vector<String> &files) {
  String cur = stripSlash(currentFile);
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == cur) return (int)i;
  return -1;
}

// Per-file hold time, falling back to the global rotate interval.
uint32_t currentHoldMs() {
  int i = plIndex(stripSlash(currentFile));
  if (i >= 0 && playlist[i].dur) return (uint32_t)playlist[i].dur * 1000UL;
  return (uint32_t)rotateSeconds * 1000UL;
}

// dir = +1 next, -1 previous. Uses shuffle when enabled.
// Skips unplayable files so one bad upload cannot stop the rotation.
void playRelative(int dir) {
  PlaybackLock guard;
  std::vector<String> files = listMediaFiles();
  if (files.empty()) { stopPlayback(true); return; }

  int n = (int)files.size();
  int idx = currentIndexIn(files);
  int target = (idx < 0) ? (dir >= 0 ? -1 : 0) : idx;

  for (int attempt = 0; attempt < n; attempt++) {
    if (shuffleMode && n > 1) {
      target = (int)random((long)n);
      if (target == idx) continue;
    } else {
      target = (target + dir + n) % n;
    }
    if (playWithFade("/" + files[target])) return;
    Serial.printf("[PLAYLIST] skipping unplayable '%s'\n", files[target].c_str());
  }

  Serial.println("[PLAYLIST] no playable file left");
  stopPlayback(true);
}

uint32_t playbackElapsedMs() {
  if (!currentFile.length() || playStartedAt == 0) return 0;
  uint32_t now = millis();
  uint32_t pausedFor = pausedAccumMs + (paused ? (now - pauseStartedAt) : 0);
  return now - playStartedAt - pausedFor;
}

void pausePlayback() {
  PlaybackLock guard;
  if (paused) return;
  if (!currentFile.length() && !showingClock) return;
  paused = true;
  pauseStartedAt = millis();
  Serial.println("[PLAY] paused");
}

void resumePlayback() {
  PlaybackLock guard;
  if (!paused) return;
  paused = false;
  pausedAccumMs += millis() - pauseStartedAt;
  nextFrameTime = millis() + 20;
  Serial.println("[PLAY] resumed");
}

void togglePause() {
  if (paused) resumePlayback(); else pausePlayback();
}

bool playFile(String path) {
  PlaybackLock guard;
  Serial.printf("\n[PLAY] '%s'\n", path.c_str());

  // Never touch the file that is currently being written: auto-rotate scans
  // the filesystem and would otherwise open a half-written GIF.
  if (uploadFile && path == uploadPath) {
    Serial.println("[PLAY] file is still uploading - skipped");
    return false;
  }

  if (!LittleFS.exists(path)) {
    Serial.println("[PLAY] file does not exist");
    return false;
  }

  File chk = LittleFS.open(path, "r");
  size_t sz = chk.size();
  chk.close();
  Serial.printf("[PLAY] size %u B\n", sz);

  stopPlayback(false, false);   // keep the old frame until the new one starts
  String lower = path;
  lower.toLowerCase();

  if (lower.endsWith(".gif")) {
    if (!isGifHeader(path)) {
      Serial.println("[PLAY] not a GIF (bad header)");
      dma_display->fillScreen(dma_display->color565(120, 0, 0));
      presentFrame();
      return false;
    }
    memset(gifCanvas, 0, sizeof(gifCanvas));
    if (gif.open(path.c_str(), gifOpen, gifCloseCB, gifRead, gifSeek, GIFDraw)) {
      Serial.printf("[PLAY] GIF OK canvas=%dx%d\n",
        gif.getCanvasWidth(), gif.getCanvasHeight());
      currentFile = path;
      playingGif = true;
      paused = false;
      pausedAccumMs = 0;
      playStartedAt = millis();
      frameCount = 0;
      nextFrameTime = millis();
      prefs.putString("last", path);
      return true;
    }
    Serial.println("[PLAY] gif.open FAILED");
    dma_display->fillScreen(dma_display->color565(120, 0, 0));
    presentFrame();
    return false;
  }

  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
    if (!jpeg.open(path.c_str(), jpegOpen, jpegCloseCB,
                   jpegRead, jpegSeek, jpegDraw)) {
      Serial.println("[PLAY] jpeg.open FAILED");
      dma_display->fillScreen(dma_display->color565(120, 0, 0));
      presentFrame();
      return false;
    }
    int w = jpeg.getWidth(), h = jpeg.getHeight();
    Serial.printf("[PLAY] JPEG %dx%d\n", w, h);
    int opt = 0;
    if      (w == 128 && h == 128) opt = JPEG_SCALE_HALF;
    else if (w == 256 && h == 256) opt = JPEG_SCALE_QUARTER;
    else if (w == 512 && h == 512) opt = JPEG_SCALE_EIGHTH;
    dma_display->clearScreen();
    bool ok = jpeg.decode(0, 0, opt);
    jpeg.close();
    showingJpeg = ok;
    if (ok) {
      presentFrame();
      currentFile = path;
      paused = false;
      pausedAccumMs = 0;
      playStartedAt = millis();
      prefs.putString("last", path);
    } else {
      dma_display->fillScreen(dma_display->color565(120, 0, 0));
      presentFrame();
    }
    return ok;
  }

  Serial.println("[PLAY] unknown extension");
  return false;
}

// Fade the panel brightness without blocking for long; used around file switches.
void fadeTo(uint8_t target, uint16_t ms) {
  uint16_t steps = ms / 15;
  if (steps < 1) steps = 1;
  int16_t from = panelBrightness;
  for (uint16_t i = 1; i <= steps; i++) {
    int16_t v = from + ((int16_t)target - from) * (int16_t)i / (int16_t)steps;
    setPanelBrightness((uint8_t)constrain(v, 0, 255));
    delay(15);
  }
  setPanelBrightness(target);
}

// Switch files through a short fade-to-black, so there is no hard cut.
bool playWithFade(String path) {
  PlaybackLock guard;
  if (!fadeEnabled || displayAsleep || panelBrightness < 8) {
    return playFile(path);
  }
  uint8_t target = effectiveBrightness();
  fadeTo(0, 150);
  bool ok = playFile(path);
  fadeTo(target, 220);
  return ok;
}

// ==================== FILENAME ====================
String sanitizeFilename(String n) {
  while (n.startsWith("/")) n = n.substring(1);
  String out = "";
  for (int i = 0; i < n.length() && out.length() < 48; i++) {
    char c = n[i];
    if (isalnum(c) || c == '.' || c == '-' || c == '_') out += c;
    else out += '_';
  }
  if (out.length() == 0) out = "file.bin";
  out.replace("..", "_");
  return out;
}

// ==================== SETTINGS (NVS) ====================
void markPrefsDirty() {
  prefsDirty = true;
  prefsSaveAt = millis() + 1500;   // debounce slider drags
}

void savePrefsNow() {
  prefs.putUChar("bright", currentBrightness);
  prefs.putBool("auto", autoRotate);
  prefs.putBool("shuffle", shuffleMode);
  prefs.putUShort("rotate", rotateSeconds);
  prefs.putUChar("pwr", powerLimit);
  prefs.putBool("fade", fadeEnabled);
  prefs.putUChar("rot", panelRotation);
  prefs.putUChar("clkF", clockFace);
  prefs.putBool("clkD", clockDate);
  prefs.putString("tz", timeZone);
  prefs.putBool("sch", schedEnabled);
  prefs.putUShort("schOn", schedOnMin);
  prefs.putUShort("schOff", schedOffMin);
  prefsDirty = false;
}

void servicePrefs() {
  if (prefsDirty && (int32_t)(millis() - prefsSaveAt) >= 0) savePrefsNow();
}

// An upload whose connection dies never delivers its final chunk, so the file
// handle would stay open for the rest of the uptime. LittleFS has very few
// descriptors; once they are gone every open() (playback included) fails.
void serviceUpload() {
  if (!uploadFile || !uploadLastAt) return;
  if (millis() - uploadLastAt < 20000) return;
  Serial.println("[UPLOAD] stale upload - closing handle");
  uploadFile.close();
  // Keep the bytes if playback somehow started on this path; deleting it under
  // the decoder would only fail anyway (open FD) and leave a corrupt entry.
  if (uploadPath.length() && uploadPath != currentFile) {
    LittleFS.remove(uploadPath);
    Serial.println("[UPLOAD] removed part file");
  }
  uploadPath = "";
  uploadBytes = 0;
  uploadError = "";
  uploadLastAt = 0;
}

// ==================== WIFI ====================
void startApFallback() {
  if (apFallback) return;
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);   // STA MAC; the radio is up by the time we get here
  char buf[24];
  snprintf(buf, sizeof(buf), "LEDMatrix-%02X%02X", mac[4], mac[5]);
  apSsid = buf;

  // Switching to AP+STA makes the wifi task allocate locks and buffers. When
  // that malloc fails the driver aborts inside lock_init_generic instead of
  // returning an error, so check the heap before asking for it.
  uint32_t freeHeap = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[WIFI] AP fallback try: heap=%u largest=%u\n",
                (unsigned)freeHeap, (unsigned)largest);
  if (freeHeap < 16000 || largest < 8000) {
    Serial.println("[WIFI] not enough RAM for the AP - staying station-only");
    apFallback = true;       // stop retrying; the STA keeps reconnecting
    lastWifiRetry = millis();
    wifiFailCount = 0;
    return;
  }

  // AP+STA so the station keeps retrying while the AP stays reachable.
  esp_log_level_set("wifi", ESP_LOG_NONE);   // an OOM error log would abort()
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  bool apOk = WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  esp_log_level_set("wifi", ESP_LOG_ERROR);
  if (!apOk) {
    Serial.println("[WIFI] softAP() failed - staying station-only");
    apFallback = true;
    lastWifiRetry = millis();
    wifiFailCount = 0;
    return;
  }
  dnsServer.start(53, "*", WiFi.softAPIP());
  apFallback = true;
  lastWifiRetry = millis();
  wifiFailCount = 0;
  Serial.printf("[WIFI] AP fallback '%s' -> http://%s\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void serviceWifi() {
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0;
    return;
  }
  uint32_t interval = apFallback ? 60000UL : 15000UL;
  if (now - lastWifiRetry < interval) return;
  lastWifiRetry = now;
  wifiFailCount++;
  Serial.println("[WIFI] reconnecting...");
  WiFi.reconnect();
  if (!apFallback && wifiFailCount >= 4) startApFallback();
}

// ==================== HELPERS ====================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c != '\r') out += c;
  }
  return out;
}

// ==================== CLOCK ====================
bool localTime(struct tm *t) {
  return getLocalTime(t, 0);
}

void drawClockFrame() {
  struct tm t;
  dma_display->clearScreen();

  if (!localTime(&t)) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(rgb565(255, 180, 0));
    dma_display->setCursor(7, 28);
    dma_display->print("Syncing");
    presentFrame();
    return;
  }

  if (clockFace == 0) {
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    dma_display->setTextSize(2);
    dma_display->setTextColor(rgb565(255, 255, 255));
    dma_display->setCursor(2, 6);
    dma_display->print(hhmm);

    if (clockDate) {
      char date[16];
      strftime(date, sizeof(date), "%a %d %b", &t);
      dma_display->setTextSize(1);
      dma_display->setTextColor(rgb565(0, 200, 255));
      int w = strlen(date) * 6;
      dma_display->setCursor((PANEL_W - w) / 2, 30);
      dma_display->print(date);
    }

    int barW = t.tm_sec * 60 / 59;
    if (barW > 0)
      dma_display->fillRect(2, 52, barW, 3, rgb565(0, 220, 120));
  } else {
    const int cx = 32, cy = 32;
    uint16_t rim = rgb565(0, 120, 200);
    for (int a = 0; a < 360; a += 5) {
      float r = a * PI / 180.0f;
      int x = cx + (int)(cosf(r) * 30);
      int y = cy + (int)(sinf(r) * 30);
      if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H)
        dma_display->drawPixel(x, y, rim);
    }
    for (int i = 0; i < 12; i++) {
      float r = i * 30 * PI / 180.0f;
      int x1 = cx + (int)(cosf(r) * 27);
      int y1 = cy + (int)(sinf(r) * 27);
      int x2 = cx + (int)(cosf(r) * 30);
      int y2 = cy + (int)(sinf(r) * 30);
      dma_display->drawLine(x1, y1, x2, y2, rim);
    }

    float ha = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f - 90.0f;
    float ma = t.tm_min * 6.0f - 90.0f;
    float sa = t.tm_sec * 6.0f - 90.0f;
    uint16_t white = rgb565(255, 255, 255);
    uint16_t red = rgb565(255, 60, 60);
    dma_display->drawLine(cx, cy,
      cx + (int)(cosf(ha * PI / 180.0f) * 15),
      cy + (int)(sinf(ha * PI / 180.0f) * 15), white);
    dma_display->drawLine(cx, cy,
      cx + (int)(cosf(ma * PI / 180.0f) * 23),
      cy + (int)(sinf(ma * PI / 180.0f) * 23), white);
    dma_display->drawLine(cx, cy,
      cx + (int)(cosf(sa * PI / 180.0f) * 26),
      cy + (int)(sinf(sa * PI / 180.0f) * 26), red);
    dma_display->fillCircle(cx, cy, 2, white);
  }
  presentFrame();
}

void startClockMode() {
  PlaybackLock guard;
  if (playingGif) { gif.close(); playingGif = false; }
  if (showingJpeg) { jpeg.close(); showingJpeg = false; }
  showingQr = false;
  paused = false;
  showingClock = true;
  lastClockTick = 0;
  drawClockFrame();
}

void stopClockMode(bool resumeMedia) {
  PlaybackLock guard;
  if (!showingClock) return;
  showingClock = false;
  clearPanel();
  if (resumeMedia) {
    String target = currentFile.length() ? currentFile : prefs.getString("last", "");
    if (target.length() && LittleFS.exists(target)) playWithFade(target);
  }
}

void serviceClock() {
  PlaybackLock guard;
  if (!showingClock) return;
  uint32_t now = millis();
  if (now - lastClockTick < (clockFace ? 250 : 500)) return;
  lastClockTick = now;
  drawClockFrame();
}

// ==================== SCHEDULE ====================
void redrawCurrentMode() {
  if (showingClock) drawClockFrame();
  else if (playingGif) pushGifCanvas();
  else if (currentFile.length()) playFile(currentFile);
  else clearPanel();
}

void checkSchedule() {
  if (!schedEnabled) {
    if (displayAsleep) {
      displayAsleep = false;
      applyBrightness();
      redrawCurrentMode();
      Serial.println("[SCHED] disabled - waking");
    }
    return;
  }

  struct tm t;
  if (!localTime(&t)) return;

  int nowMin = t.tm_hour * 60 + t.tm_min;
  bool shouldSleep;
  if (schedOnMin <= schedOffMin)
    shouldSleep = (nowMin < schedOnMin || nowMin >= schedOffMin);
  else
    shouldSleep = (nowMin >= schedOffMin && nowMin < schedOnMin);

  if (shouldSleep == displayAsleep) return;

  displayAsleep = shouldSleep;
  applyBrightness();
  if (displayAsleep) {
    clearPanel();
    Serial.println("[SCHED] display off");
  } else {
    redrawCurrentMode();
    Serial.println("[SCHED] display on");
  }
}

// ==================== STATUS / AUTH HELPERS ====================
String buildStatusJson() {
  PlaybackLock guard;
  String type = "none";
  if (showingClock) type = "clock";
  else if (showingQr) type = "qr";
  else if (playingGif) type = "gif";
  else if (showingJpeg) type = "jpeg";

  std::vector<String> files = listMediaFiles();
  int idx = currentIndexIn(files);

  String json = "{";
  json.reserve(900);
  json += "\"file\":\"" + currentFile + "\",";
  json += "\"type\":\"" + type + "\",";
  json += "\"brightness\":" + String(currentBrightness) + ",";
  json += "\"paused\":" + String(paused ? "true" : "false") + ",";
  json += "\"auto\":" + String(autoRotate ? "true" : "false") + ",";
  json += "\"shuffle\":" + String(shuffleMode ? "true" : "false") + ",";
  json += "\"rotate\":" + String(rotateSeconds) + ",";
  json += "\"fps\":" + String(currentFps) + ",";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"fsUsed\":" + String((uint32_t)LittleFS.usedBytes()) + ",";
  json += "\"fsTotal\":" + String((uint32_t)LittleFS.totalBytes()) + ",";
  json += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"ap\":" + String(apFallback ? "true" : "false") + ",";
  json += "\"apSsid\":\"" + apSsid + "\",";
  json += "\"apIp\":\"" + (apFallback ? WiFi.softAPIP().toString() : String("")) + "\",";
  json += "\"rot\":" + String(panelRotation) + ",";
  json += "\"clockFace\":" + String(clockFace) + ",";
  json += "\"clockDate\":" + String(clockDate ? "true" : "false") + ",";
  json += "\"tz\":\"" + timeZone + "\",";
  json += "\"time\":" + String((long)time(nullptr)) + ",";
  json += "\"sched\":" + String(schedEnabled ? "true" : "false") + ",";
  json += "\"schedOn\":" + String(schedOnMin) + ",";
  json += "\"schedOff\":" + String(schedOffMin) + ",";
  json += "\"asleep\":" + String(displayAsleep ? "true" : "false") + ",";
  json += "\"pwr\":" + String(powerLimit) + ",";
  json += "\"fade\":" + String(fadeEnabled ? "true" : "false") + ",";
  json += "\"pin\":" + String(adminPin.length() ? "true" : "false") + ",";
  json += "\"files\":" + String((unsigned)files.size()) + ",";
  json += "\"index\":" + String(idx < 0 ? 0 : idx + 1);
  json += "}";
  return json;
}

// True when the request carries a valid session cookie or the PIN itself.
bool authed(AsyncWebServerRequest *r) {
  if (!adminPin.length()) return true;
  if (r->hasParam("pin") && r->getParam("pin")->value() == adminPin) return true;
  if (r->hasHeader("Cookie")) {
    String c = r->header("Cookie");
    int i = c.indexOf("auth=");
    if (i >= 0) {
      int e = c.indexOf(';', i);
      String v = (e > 0) ? c.substring(i + 5, e) : c.substring(i + 5);
      if (v == authToken) return true;
    }
  }
  return false;
}

// Returns true when the request was rejected and a response already sent.
bool gate(AsyncWebServerRequest *r) {
  if (authed(r)) return false;
  // Do not build a big response when RAM is nearly gone (the header list is a
  // throwing std::list allocation). abort() needs no memory and is the
  // library's supported way to drop a request from a handler.
  if (ESP.getFreeHeap() < 12000 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 2000) {
    r->abort();
    return true;
  }
  r->send(401, "text/plain", "PIN required");
  return true;
}

bool wsIsAuthed(uint32_t id) {
  for (int i = 0; i < 4; i++) if (wsAuth[i] == id && id) return true;
  return false;
}

void wsSetAuthed(uint32_t id) {
  for (int i = 0; i < 4; i++) if (wsAuth[i] == id || wsAuth[i] == 0) { wsAuth[i] = id; return; }
  wsAuth[0] = id;
}

void wsClearAuthed(uint32_t id) {
  for (int i = 0; i < 4; i++) if (wsAuth[i] == id) wsAuth[i] = 0;
}

void broadcastStatus() {
  if (!ws.count()) return;
  String json = buildStatusJson();
  for (auto &c : ws.getClients())
    if (c.status() == WS_CONNECTED && wsIsAuthed(c.id())) c.text(json);
}

// ---- HTTP wedged watchdog ------------------------------------------------
// All HTTP/WS traffic runs in one AsyncTCP task. If a handler ever blocks
// forever, port 80 stays open (ping and mDNS still answer) but nothing is
// served and only a reset helps. The UI is pushed a status message every
// second, so when that client's transmit queue has been full for a long time
// the task cannot be draining it any more: reboot into a clean state.
uint32_t txStuckSince = 0;

void serviceHttpWatchdog() {
  bool full = false;
  for (auto &c : ws.getClients())
    if (c.status() == WS_CONNECTED && c.queueIsFull()) full = true;
  if (!full) { txStuckSince = 0; return; }
  if (!txStuckSince) {
    txStuckSince = millis();
    Serial.println("[WD] websocket backlog started");
    return;
  }
  if (millis() - txStuckSince > 40000) {
    Serial.println("[WD] async task wedged - restarting");
    Serial.flush();
    restartNow();
  }
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DISCONNECT) {
    wsClearAuthed(c->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (!info || !info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT || len < 8 || len > 40) { c->close(); return; }
    String tok((const char*)data, len);
    if (tok == authToken) {
      wsSetAuthed(c->id());
      c->text(buildStatusJson());
    } else {
      c->close();
    }
  }
}

// ==================== QR SCREEN ====================
String uiUrl() {
  String host = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                                : WiFi.softAPIP().toString();
  return "http://" + host;
}

void drawQrScreen(const String &url) {
  PlaybackLock guard;
  if (playingGif) { gif.close(); playingGif = false; }
  if (showingJpeg) { jpeg.close(); showingJpeg = false; }
  showingClock = false;
  paused = false;

  static QRCode qr;
  static uint8_t qrBuf[128];
  dma_display->clearScreen();

  if (qrcode_initText(&qr, qrBuf, 2, ECC_LOW, url.c_str()) != 0) {
    dma_display->setTextColor(rgb565(255, 80, 80));
    dma_display->setTextSize(1);
    dma_display->setCursor(2, 28);
    dma_display->print("QR error");
    presentFrame();
    return;
  }

  // 25 modules * 2 px = 50 px, 7 px quiet zone on each side.
  const int scale = 2, off = (PANEL_W - qr.size * scale) / 2;
  uint16_t on = rgb565(255, 255, 255);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        dma_display->fillRect(off + x * scale, off + y * scale, scale, scale, on);

  showingQr = true;   // handled last so the GIF loop stops drawing over it
  qrUntil = millis() + 15000;
  presentFrame();
}

void stopQrScreen() {
  if (!showingQr) return;
  showingQr = false;
}

// ==================== HTML ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Matrix 64×64</title>
<style>
:root{--bg:#0e1116;--card:#1a1f27;--border:#2a313c;--accent:#22c55e;--danger:#ef4444;--blue:#3b82f6;--muted:#94a3b8}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:#e2e8f0;
     max-width:600px;margin:0 auto;padding:1em;line-height:1.5}
h1{font-size:1.4em;margin:.2em 0 .8em}
h2{font-size:1em;color:var(--muted);margin:1.2em 0 .5em;font-weight:500;text-transform:uppercase;letter-spacing:.05em}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1em;margin-bottom:.8em}
button{font:inherit;padding:.6em 1.1em;border:0;border-radius:6px;cursor:pointer;
       background:var(--accent);color:#fff;font-weight:500;transition:opacity .15s}
button:hover{opacity:.85}
button:disabled{opacity:.4;cursor:wait}
button.secondary{background:var(--blue)}
button.danger{background:var(--danger)}
button.ghost{background:transparent;border:1px solid var(--border);color:#e2e8f0}
.row{display:flex;gap:.5em;flex-wrap:wrap;align-items:center}
input[type=file]{flex:1;min-width:150px;color:#e2e8f0;padding:.4em;border:1px dashed var(--border);
                 border-radius:6px;background:transparent;cursor:pointer}
input[type=range]{flex:1;width:100%}
select{font:inherit;padding:.45em .6em;border-radius:6px;background:#0f1720;color:#e2e8f0;
       border:1px solid var(--border);cursor:pointer}
#status{padding:.7em 1em;background:#0f1720;border-radius:6px;margin:.8em 0;
        border-left:3px solid var(--blue);font-size:.9em;min-height:2.5em}
#status.error{border-color:var(--danger)}
#status.ok{border-color:var(--accent)}
#status.warn{border-color:#f59e0b}
#fileInfo{font-size:.85em;color:var(--muted);padding:.3em 0;min-height:1.2em}
#files .file{display:flex;align-items:center;gap:.35em;padding:.4em 0;
             border-bottom:1px solid var(--border);flex-wrap:wrap}
#files .file:last-child{border-bottom:0}
#files .file.off{opacity:.42}
#files .name{flex:1;min-width:5em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:.9em}
#files .size{color:var(--muted);font-size:.78em;margin-left:.3em}
#files .playing{color:var(--accent);font-weight:500}
#files button{padding:.25em .55em;font-size:.8em;line-height:1.3}
#files input.dur{width:3.4em;padding:.25em .3em;font-size:.8em;text-align:center;
  -moz-appearance:textfield}
#files input.dur::-webkit-outer-spin-button,
#files input.dur::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
.empty{color:var(--muted);font-size:.9em;padding:1em 0;text-align:center}
.progress{height:4px;background:var(--border);border-radius:2px;overflow:hidden;margin-top:.5em}
.progress-bar{height:100%;background:var(--accent);width:0%;transition:width .2s}
.preview{display:block;margin:.8em auto 0;background:#000;border:1px solid var(--border);
         border-radius:6px;image-rendering:pixelated;image-rendering:crisp-edges;
         width:128px;height:128px}
.preview-label{text-align:center;font-size:.78em;color:var(--muted);margin-top:.25em}
#cropArea{margin-top:.8em;text-align:center}
.crop-hint{font-size:.8em;color:var(--muted);margin:.3em 0}
#cropCanvas{display:block;margin:0 auto;border:1px solid var(--border);border-radius:6px;
            cursor:crosshair;touch-action:none;max-width:100%;height:auto;background:#000}
#cropInfo{font-size:.8em;color:var(--muted)}
#clearCropBtn{padding:.3em .7em;font-size:.8em}
.ctl{display:flex;gap:.5em;flex-wrap:wrap}
.ctl button{flex:1;min-width:78px}
.toggle{display:flex;align-items:center;gap:.45em;font-size:.88em;cursor:pointer}
.toggle input{width:1.05em;height:1.05em;accent-color:var(--accent);cursor:pointer}
input[type=text],input[type=password],input[type=number],input[type=time]{font:inherit;padding:.45em .6em;
  border-radius:6px;background:#0f1720;color:#e2e8f0;border:1px solid var(--border);min-width:0}
input[type=color]{width:2.7em;height:2.2em;padding:0;border:1px solid var(--border);
  border-radius:6px;background:#0f1720;cursor:pointer}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:.15em 1em;font-size:.85em;margin-top:.7em}
.stats div{display:flex;justify-content:space-between;gap:.4em;padding:.18em 0;
  border-bottom:1px dotted var(--border);min-width:0}
.stats span:first-child{color:var(--muted)}
.stats span:last-child{color:var(--accent);font-variant-numeric:tabular-nums;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:60%}
#wifiInfo{font-size:.85em;color:var(--muted);word-break:break-word}
details summary{cursor:pointer;font-size:.85em;color:var(--muted);padding:.2em 0}
#login{display:none;position:fixed;inset:0;background:rgba(8,11,15,.92);z-index:50;
  align-items:center;justify-content:center;padding:1em}
#login.on{display:flex}
.login-card{background:var(--card);border:1px solid var(--border);border-radius:12px;
  padding:1.2em;max-width:320px;width:100%}
.login-card h3{margin:0 0 .4em;font-size:1.05em}
.login-card p{margin:0 0 .8em;font-size:.85em;color:var(--muted)}
.login-card input{width:100%;font-size:1.1em;letter-spacing:.15em;text-align:center}
</style></head><body>

<h1>🎨 LED Matrix 64×64</h1>
<div id="status">Ready.</div>

<h2>Upload a file (auto-convert to 64×64)</h2>
<div class="card">
  <div class="row">
    <input type="file" id="file" multiple accept=".gif,.jpg,.jpeg,.png,.webp,.bmp">
    <button id="uploadBtn">⬆ Upload</button>
  </div>
  <div id="fileInfo"></div>
  <div class="progress" id="prog" style="display:none"><div class="progress-bar"></div></div>
  <div class="row" style="margin-top:.6em">
    <label for="fitMode" style="font-size:.85em;color:var(--muted)">Scaling:</label>
    <select id="fitMode">
      <option value="fit">Fit - black bars</option>
      <option value="fill">Fill - crop</option>
    </select>
  </div>
  <div class="row" style="margin-top:.4em">
    <label for="frameDelay" style="font-size:.85em;color:var(--muted)">Delay:</label>
    <input type="range" id="frameDelay" min="0" max="500" step="10" value="0">
    <span id="frameDelayVal" style="min-width:4.5em;text-align:right;font-size:.85em;color:var(--muted)">from file</span>
  </div>

  <div id="cropArea" style="display:none">
    <div class="crop-hint">Drag on the source to select crop area</div>
    <canvas id="cropCanvas"></canvas>
    <div class="row" style="justify-content:center;margin-top:.4em;gap:.5em">
      <span id="cropInfo">Full image</span>
      <button class="ghost" id="clearCropBtn" style="display:none">Clear crop</button>
    </div>
  </div>

  <canvas id="preview" class="preview" width="64" height="64" style="display:none"
          title="Live 64×64 preview, 2× zoom"></canvas>
  <div class="preview-label" id="previewLabel" style="display:none">Live preview — 64×64, 2× zoom</div>
</div>

<h2>Playback</h2>
<div class="card">
  <div class="ctl">
    <button class="ghost" id="prevBtn">⏮ Prev</button>
    <button id="pauseBtn">⏸ Pause</button>
    <button class="ghost" id="nextBtn">Next ⏭</button>
  </div>
  <label class="toggle"><input type="checkbox" id="autoRot"> Auto-rotate playlist</label>
  <div class="row" style="margin-top:.5em">
    <label class="toggle"><input type="checkbox" id="shuffle"> Shuffle</label>
    <label for="rotateSec" style="font-size:.85em;color:var(--muted);margin-left:auto">Every</label>
    <input type="number" id="rotateSec" min="5" max="3600" step="5" value="30" style="width:5em">
    <span style="font-size:.85em;color:var(--muted)">s</span>
  </div>
</div>

<h2>Clock</h2>
<div class="card">
  <div class="row">
    <select id="clkFace">
      <option value="0">Digital</option>
      <option value="1">Analog</option>
    </select>
    <label class="toggle"><input type="checkbox" id="clkDate" checked> Date</label>
    <button id="clkStart">▶ Show</button>
    <button class="ghost" id="clkStop">■ Stop</button>
  </div>
  <div class="row" style="margin-top:.5em">
    <label for="clkTz" style="font-size:.85em;color:var(--muted)">Time zone</label>
    <select id="clkTz" style="flex:1;min-width:9em">
      <option value="UTC0">UTC</option>
      <option value="GMT0BST,M3.5.0/1,M10.5.0">UK (London)</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
      <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
      <option value="IST-5:30">India</option>
      <option value="JST-9">Japan</option>
      <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia East</option>
      <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand</option>
    </select>
  </div>
</div>

<h2>Schedule</h2>
<div class="card">
  <label class="toggle"><input type="checkbox" id="schOn"> Turn display off outside this window</label>
  <div class="row" style="margin-top:.5em">
    <label for="schFrom" style="font-size:.85em;color:var(--muted)">On at</label>
    <input type="time" id="schFrom" value="08:00">
    <label for="schTo" style="font-size:.85em;color:var(--muted)">Off at</label>
    <input type="time" id="schTo" value="23:00">
    <button class="secondary" id="schSave">Save</button>
  </div>
  <div style="font-size:.78em;color:var(--muted);margin-top:.45em">
    Needs the clock to be synced. Overnight windows (e.g. 18:00 → 02:00) work as expected.
  </div>
</div>

<h2>Panel &amp; color</h2>
<div class="card">
  <div class="row">
    <label for="pwrLimit" style="font-size:.85em;color:var(--muted)">Power limit</label>
    <input type="range" id="pwrLimit" min="20" max="100" step="5" value="100">
    <span id="pwrVal" style="min-width:3em;text-align:right">100%</span>
  </div>
  <div class="row" style="margin-top:.5em">
    <label for="rotSel" style="font-size:.85em;color:var(--muted)">Rotation</label>
    <select id="rotSel">
      <option value="0">0&deg;</option>
      <option value="1">90&deg;</option>
      <option value="2">180&deg;</option>
      <option value="3">270&deg;</option>
    </select>
  </div>
  <label class="toggle" style="margin-top:.45em"><input type="checkbox" id="fadeTog" checked> Fade between files</label>
  <div style="font-size:.78em;color:var(--muted);margin-top:.45em">
    Lower the power limit if the panel browns out on bright white scenes.
  </div>
</div>

<h2>Brightness</h2>
<div class="card">
  <div class="row">
    <input type="range" id="bright" min="5" max="255" value="90">
    <span id="brightVal" style="min-width:3em;text-align:right">90</span>
  </div>
</div>

<h2>Files on device</h2>
<div class="card">
  <div class="row" style="margin-bottom:.4em">
    <span style="font-size:.78em;color:var(--muted);flex:1">
      Order and hold time are saved; 0 = use the global interval.
    </span>
    <button class="danger" id="delAll">🗑 Delete all</button>
  </div>
  <div id="files"><div class="empty">loading...</div></div>
</div>

<h2>Device</h2>
<div class="card">
  <div id="wifiInfo">—</div>
  <div class="stats">
    <div><span>Playing</span><span id="stFile">—</span></div>
    <div><span>Playlist</span><span id="stPl">—</span></div>
    <div><span>FPS</span><span id="stFps">—</span></div>
    <div><span>Storage</span><span id="stFs">—</span></div>
    <div><span>Free heap</span><span id="stHeap">—</span></div>
    <div><span>Wi-Fi signal</span><span id="stRssi">—</span></div>
    <div><span>Uptime</span><span id="stUp">—</span></div>
  </div>
  <div class="progress" style="margin-top:.6em"><div class="progress-bar" id="fsBar" style="width:0%"></div></div>
  <div class="row" style="margin-top:.6em">
    <button class="secondary" id="qrBtn">📱 Show QR</button>
    <button class="ghost" id="qrBoot">QR on boot</button>
    <button class="ghost" id="cfgBtn">⤓ Config</button>
    <button class="ghost" id="diagBtn">🩺 Diag</button>
    <button class="ghost" id="resetBtn">↺ Reset</button>
  </div>
  <pre id="diagOut" style="display:none;font-size:.72em;line-height:1.45;white-space:pre-wrap;
    word-break:break-word;color:var(--muted);margin:.7em 0 0"></pre>
  <details style="margin-top:.7em">
    <summary>Security (PIN)</summary>
    <div class="row" style="margin-top:.5em">
      <input type="password" id="pinSet" placeholder="New PIN (4-16 chars)" style="flex:1;min-width:130px">
      <button class="secondary" id="pinSave">Set</button>
      <button class="ghost" id="pinClear">Remove</button>
    </div>
    <div id="pinState" style="font-size:.8em;color:var(--muted);margin-top:.4em">—</div>
  </details>
  <details style="margin-top:.7em">
    <summary>Change Wi-Fi</summary>
    <div class="row" style="margin-top:.5em">
      <input type="text" id="wifiSsid" placeholder="Network name" style="flex:1;min-width:120px">
      <input type="password" id="wifiPass" placeholder="Password" style="flex:1;min-width:120px">
      <button class="secondary" id="wifiSave">Save &amp; reboot</button>
    </div>
  </details>
</div>

<div class="row">
  <button class="danger" onclick="stop()">■ Stop</button>
  <button class="ghost" onclick="refresh()">🔄 Refresh</button>
</div>

<div id="login">
  <div class="login-card">
    <h3>🔒 PIN required</h3>
    <p>Enter the device PIN to unlock the controls.</p>
    <input type="password" id="pinEntry" inputmode="numeric" autocomplete="off" placeholder="PIN">
    <div class="row" style="margin-top:.6em">
      <button id="pinGo" style="flex:1">Unlock</button>
    </div>
    <div id="pinMsg" style="font-size:.85em;color:var(--danger);min-height:1.2em;margin-top:.4em"></div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
const st = $('status');
const prog = $('prog');
const bar = prog.querySelector('.progress-bar');
const fileInfo = $('fileInfo');
let panelInit = false;

// ============ AUTH / LIVE STATUS ============
let ws = null, wsOk = false, loginShown = false;

function showLogin(msg) {
  if (!loginShown) { loginShown = true; $('login').classList.add('on'); }
  if (msg) $('pinMsg').textContent = msg;
}

// Every request goes through here so a 401 anywhere brings up the PIN prompt.
const rawFetch = window.fetch.bind(window);
window.fetch = async (...a) => {
  const r = await rawFetch(...a);
  if (r.status === 401) showLogin();
  return r;
};

$('pinGo').onclick = async () => {
  const p = $('pinEntry').value.trim();
  if (!p) { $('pinMsg').textContent = 'Enter the PIN.'; return; }
  const r = await rawFetch('/auth?pin=' + encodeURIComponent(p));
  if (r.ok) { location.reload(); return; }
  $('pinMsg').textContent = 'Wrong PIN, try again.';
  $('pinEntry').value = '';
};
$('pinEntry').addEventListener('keydown', e => {
  if (e.key === 'Enter') $('pinGo').click();
});

async function startWs() {
  try {
    const r = await rawFetch('/authstate');
    if (!r.ok) { showLogin(); return; }
    const a = await r.json();
    ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') +
                       location.host + '/ws');
    ws.onopen = () => ws.send(a.token);
    ws.onmessage = ev => {
      wsOk = true;
      try { applyStatus(JSON.parse(ev.data)); } catch(e) {}
    };
    ws.onclose = () => { wsOk = false; };
    ws.onerror = () => { wsOk = false; };
  } catch(e) { /* polling covers it */ }
}

// Inter-frame delay slider.
const delaySlider = $('frameDelay');
const delayLabel = $('frameDelayVal');

function customDelay() {
  const v = parseInt(delaySlider.value, 10) || 0;
  return v >= 20 ? v : 0;
}
function updateDelayLabel() {
  const v = customDelay();
  delayLabel.textContent = v ? v + ' ms' : 'from file';
  clearTimeout(updateDelayLabel.timer);
  updateDelayLabel.timer = setTimeout(() => {
    updateFileInfo();
    reschedulePreviewWithNewDelay();
  }, 150);
}
delaySlider.oninput = updateDelayLabel;
updateDelayLabel();

function setStatus(msg, type='') {
  st.textContent = msg;
  st.className = type;
}
function fmtSize(b) {
  if (b < 1024) return b + ' B';
  if (b < 1024*1024) return (b/1024).toFixed(1) + ' KB';
  return (b/1024/1024).toFixed(1) + ' MB';
}

// ============ CONVERSION ============
// Built-in GIF codec (decoder + encoder, zero external dependencies).

(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.GifCodec = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
'use strict';

function u16(b, p) { return b[p] | (b[p + 1] << 8); }

function skipSubBlocks(b, p) {
  for (;;) {
    if (p >= b.length) return p;
    const n = b[p++];
    if (n === 0) return p;
    p += n;
  }
}

function readSubBlocks(b, p) {
  const chunks = [];
  let total = 0;
  for (;;) {
    if (p >= b.length) break;
    const n = b[p++];
    if (n === 0) break;
    chunks.push(b.subarray(p, p + n));
    total += n;
    p += n;
  }
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return { data: out, pos: p };
}

function deinterlace(idx, w, h) {
  const out = new Uint8Array(w * h);
  const rows = [];
  for (let y = 0; y < h; y += 8) rows.push(y);
  for (let y = 4; y < h; y += 8) rows.push(y);
  for (let y = 2; y < h; y += 4) rows.push(y);
  for (let y = 1; y < h; y += 2) rows.push(y);
  let s = 0;
  for (let r = 0; r < rows.length; r++) {
    out.set(idx.subarray(s, s + w), rows[r] * w);
    s += w;
  }
  return out;
}

function lzwDecode(minCodeSize, data, pixelCount) {
  const CLEAR = 1 << minCodeSize;
  const EOI = CLEAR + 1;
  const out = new Uint8Array(pixelCount);

  const prefix = new Int32Array(4096);
  const suffix = new Uint8Array(4096);
  const stack = new Uint8Array(4096);

  for (let i = 0; i < CLEAR; i++) { prefix[i] = -1; suffix[i] = i; }

  let codeSize = minCodeSize + 1;
  let nextCode = EOI + 1;
  let old = -1;
  let first = 0;
  let outPos = 0;
  let bitBuf = 0;
  let bitCnt = 0;
  let p = 0;

  while (outPos < pixelCount) {
    while (bitCnt < codeSize) {
      if (p >= data.length) return out;
      bitBuf |= data[p++] << bitCnt;
      bitCnt += 8;
    }
    const code = bitBuf & ((1 << codeSize) - 1);
    bitBuf >>>= codeSize;
    bitCnt -= codeSize;

    if (code === CLEAR) {
      codeSize = minCodeSize + 1;
      nextCode = EOI + 1;
      old = -1;
      continue;
    }
    if (code === EOI) break;

    if (old === -1) {
      if (code >= CLEAR) break;
      out[outPos++] = suffix[code];
      first = suffix[code];
      old = code;
      continue;
    }

    const inCode = code;
    let sp = 0;
    let c = code;

    if (c >= nextCode) {
      stack[sp++] = first;
      c = old;
    }
    while (c >= CLEAR && sp < stack.length) {
      stack[sp++] = suffix[c];
      c = prefix[c];
      if (c < 0) break;
    }
    if (c < 0) break;
    first = suffix[c];
    stack[sp++] = first;

    if (nextCode < 4096) {
      prefix[nextCode] = old;
      suffix[nextCode] = first;
      nextCode++;
      if (nextCode === (1 << codeSize) && codeSize < 12) codeSize++;
    }

    old = inCode;
    while (sp > 0 && outPos < pixelCount) out[outPos++] = stack[--sp];
  }
  return out;
}

function lzwEncode(minCodeSize, pixels) {
  const CLEAR = 1 << minCodeSize;
  const EOI = CLEAR + 1;
  const out = [];

  let codeSize = minCodeSize + 1;
  let maxCode = (1 << codeSize) - 1;
  let nextCode = EOI + 1;
  let dict = new Map();
  let bitBuf = 0;
  let bitCnt = 0;

  function emit(code) {
    bitBuf |= code << bitCnt;
    bitCnt += codeSize;
    while (bitCnt >= 8) {
      out.push(bitBuf & 0xFF);
      bitBuf >>>= 8;
      bitCnt -= 8;
    }
  }

  function reset() {
    dict = new Map();
    codeSize = minCodeSize + 1;
    maxCode = (1 << codeSize) - 1;
    nextCode = EOI + 1;
  }

  emit(CLEAR);

  let prefix = pixels[0];
  for (let i = 1; i < pixels.length; i++) {
    const k = pixels[i];
    const key = (prefix << 8) | k;
    const found = dict.get(key);
    if (found !== undefined) { prefix = found; continue; }

    emit(prefix);
    if (nextCode > maxCode && codeSize < 12) {
      codeSize++;
      maxCode = (1 << codeSize) - 1;
    }
    if (nextCode < 4096) {
      dict.set(key, nextCode++);
    } else {
      emit(CLEAR);
      reset();
    }
    prefix = k;
  }
  emit(prefix);
  emit(EOI);
  if (bitCnt > 0) out.push(bitBuf & 0xFF);
  return out;
}

function decode(input, onFrame) {
  const b = (input instanceof Uint8Array) ? input : new Uint8Array(input);
  if (b.length < 14) throw new Error('GIF file too short');

  const sig = String.fromCharCode(b[0], b[1], b[2], b[3], b[4], b[5]);
  if (sig !== 'GIF87a' && sig !== 'GIF89a') throw new Error('Not a GIF file');

  const width = u16(b, 6);
  const height = u16(b, 8);
  if (!width || !height) throw new Error('GIF has zero dimensions');

  const packed = b[10];
  let p = 13;
  let gct = null;
  if (packed & 0x80) {
    const n = 1 << ((packed & 7) + 1);
    gct = b.subarray(p, p + n * 3);
    p += n * 3;
  }

  const canvas = new Uint8ClampedArray(width * height * 4);
  for (let i = 3; i < canvas.length; i += 4) canvas[i] = 255;

  let gce = { delay: 0, disposal: 0, trans: -1, hasTrans: false };
  let prevDisposal = 0;
  let prevRect = null;
  let saved = null;
  let loop = 0;
  let frameCount = 0;

  while (p < b.length) {
    const block = b[p++];

    if (block === 0x3B) break;

    if (block === 0x21) {
      const label = b[p++];

      if (label === 0xF9) {
        const size = b[p++];
        const pk = b[p];
        gce.disposal = (pk >> 2) & 7;
        gce.hasTrans = (pk & 1) === 1;
        gce.delay = u16(b, p + 1);
        gce.trans = b[p + 3];
        p += size;
        p = skipSubBlocks(b, p);
      } else if (label === 0xFF) {
        const size = b[p++];
        const app = String.fromCharCode.apply(null, b.subarray(p, p + size));
        p += size;
        if (app.indexOf('NETSCAPE') === 0) {
          const sub = readSubBlocks(b, p);
          if (sub.data.length >= 3 && sub.data[0] === 1) loop = u16(sub.data, 1);
          p = sub.pos;
        } else {
          p = skipSubBlocks(b, p);
        }
      } else {
        p = skipSubBlocks(b, p);
      }
      continue;
    }

    if (block !== 0x2C) break;

    const left = u16(b, p); p += 2;
    const top = u16(b, p); p += 2;
    const fw = u16(b, p); p += 2;
    const fh = u16(b, p); p += 2;
    const ipk = b[p++];
    let pal = gct;
    if (ipk & 0x80) {
      const n = 1 << ((ipk & 7) + 1);
      pal = b.subarray(p, p + n * 3);
      p += n * 3;
    }
    if (!pal) throw new Error('GIF without a color table');
    const interlaced = (ipk & 0x40) !== 0;

    const minCodeSize = b[p++];
    const sub = readSubBlocks(b, p);
    p = sub.pos;

    let idx = lzwDecode(minCodeSize, sub.data, fw * fh);
    if (interlaced) idx = deinterlace(idx, fw, fh);

    if (prevDisposal === 2 && prevRect) {
      const { x, y, w, h } = prevRect;
      for (let yy = y; yy < y + h && yy < height; yy++) {
        if (yy < 0) continue;
        let di = (yy * width + Math.max(0, x)) * 4;
        for (let xx = Math.max(0, x); xx < x + w && xx < width; xx++, di += 4) {
          canvas[di] = 0; canvas[di + 1] = 0; canvas[di + 2] = 0; canvas[di + 3] = 255;
        }
      }
    } else if (prevDisposal === 3 && saved) {
      canvas.set(saved);
    }

    if (gce.disposal === 3) saved = canvas.slice();

    const trans = gce.hasTrans ? gce.trans : -1;
    for (let y = 0; y < fh; y++) {
      const dy = top + y;
      if (dy < 0 || dy >= height) continue;
      let si = y * fw;
      let di = (dy * width + left) * 4;
      for (let x = 0; x < fw; x++, si++, di += 4) {
        const dx = left + x;
        if (dx < 0 || dx >= width) continue;
        const c = idx[si];
        if (c === trans) continue;
        const q = c * 3;
        canvas[di] = pal[q];
        canvas[di + 1] = pal[q + 1];
        canvas[di + 2] = pal[q + 2];
        canvas[di + 3] = 255;
      }
    }

    const cs = gce.delay < 2 ? 10 : gce.delay;

    onFrame({
      rgba: canvas,
      width: width,
      height: height,
      delay: cs * 10
    });
    frameCount++;

    prevDisposal = gce.disposal;
    prevRect = { x: left, y: top, w: fw, h: fh };
    gce = { delay: 0, disposal: 0, trans: -1, hasTrans: false };
  }

  return { width: width, height: height, loop: loop, frames: frameCount };
}

function quantize(frames, maxColors) {
  const hist = new Uint32Array(1 << 18);
  for (let f = 0; f < frames.length; f++) {
    const d = frames[f].rgba;
    for (let i = 0; i < d.length; i += 4) {
      const k = ((d[i] >> 2) << 12) | ((d[i + 1] >> 2) << 6) | (d[i + 2] >> 2);
      hist[k]++;
    }
  }

  let bins = 0;
  for (let k = 0; k < hist.length; k++) if (hist[k]) bins++;
  if (!bins) bins = 1;

  const cr = new Uint8Array(bins);
  const cg = new Uint8Array(bins);
  const cb = new Uint8Array(bins);
  const cw = new Uint32Array(bins);
  const order = new Uint32Array(bins);

  let j = 0;
  for (let k = 0; k < hist.length; k++) {
    if (!hist[k]) continue;
    cr[j] = (((k >> 12) & 63) << 2) | 2;
    cg[j] = (((k >> 6) & 63) << 2) | 2;
    cb[j] = ((k & 63) << 2) | 2;
    cw[j] = hist[k];
    order[j] = j;
    j++;
  }

  if (bins > maxColors) {
    const boxes = [{ start: 0, end: bins, ch: 0, range: 255 }];
    const rangeOf = (bx) => {
      let rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
      for (let o = bx.start; o < bx.end; o++) {
        const c = order[o];
        const r = cr[c], g = cg[c], bl = cb[c];
        if (r < rmin) rmin = r; if (r > rmax) rmax = r;
        if (g < gmin) gmin = g; if (g > gmax) gmax = g;
        if (bl < bmin) bmin = bl; if (bl > bmax) bmax = bl;
      }
      const rr = rmax - rmin, gr = gmax - gmin, br = bmax - bmin;
      if (rr >= gr && rr >= br) { bx.ch = 0; bx.range = rr; }
      else if (gr >= br) { bx.ch = 1; bx.range = gr; }
      else { bx.ch = 2; bx.range = br; }
      return bx.range;
    };

    for (let i = 0; i < boxes.length; i++) rangeOf(boxes[i]);

    while (boxes.length < maxColors) {
      let best = -1, bestScore = 0;
      for (let i = 0; i < boxes.length; i++) {
        if (boxes[i].end - boxes[i].start < 2 || boxes[i].range <= 0) continue;
        const score = boxes[i].range;
        if (score > bestScore) { bestScore = score; best = i; }
      }
      if (best < 0) break;

      const bx = boxes[best];
      const ch = bx.ch;
      const arr = Array.prototype.slice.call(order.subarray(bx.start, bx.end));
      const key = ch === 0 ? cr : ch === 1 ? cg : cb;
      arr.sort(function (a, b) { return key[a] - key[b]; });

      let total = 0;
      for (let i = 0; i < arr.length; i++) total += cw[arr[i]];

      let acc = 0, cut = 0;
      for (; cut < arr.length - 1; cut++) {
        acc += cw[arr[cut]];
        if (acc * 2 >= total) break;
      }
      if (cut >= arr.length - 1) cut = arr.length - 2;

      for (let i = 0; i < arr.length; i++) order[bx.start + i] = arr[i];

      const mid = bx.start + cut + 1;
      const left = { start: bx.start, end: mid };
      const right = { start: mid, end: bx.end };
      rangeOf(left); rangeOf(right);
      boxes[best] = left;
      boxes.push(right);
    }

    const pal = new Uint8Array(boxes.length * 3);
    for (let i = 0; i < boxes.length; i++) {
      const bx = boxes[i];
      let sr = 0, sg = 0, sb = 0, sw = 0;
      for (let o = bx.start; o < bx.end; o++) {
        const c = order[o], w = cw[c];
        sr += cr[c] * w; sg += cg[c] * w; sb += cb[c] * w; sw += w;
      }
      if (!sw) sw = 1;
      pal[i * 3] = Math.round(sr / sw);
      pal[i * 3 + 1] = Math.round(sg / sw);
      pal[i * 3 + 2] = Math.round(sb / sw);
    }
    return { palette: pal, count: boxes.length, cr: cr, cg: cg, cb: cb };
  }

  const pal = new Uint8Array(bins * 3);
  for (let i = 0; i < bins; i++) {
    pal[i * 3] = cr[i]; pal[i * 3 + 1] = cg[i]; pal[i * 3 + 2] = cb[i];
  }
  return { palette: pal, count: bins, cr: cr, cg: cg, cb: cb };
}

function buildLut(pal, count) {
  const lut = new Int16Array(1 << 18).fill(-1);
  const pr = new Uint8Array(count), pg = new Uint8Array(count), pb = new Uint8Array(count);
  for (let i = 0; i < count; i++) {
    pr[i] = pal[i * 3]; pg[i] = pal[i * 3 + 1]; pb[i] = pal[i * 3 + 2];
  }
  return function lookup(r, g, b) {
    const k = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
    let v = lut[k];
    if (v >= 0) return v;
    const br = (((k >> 12) & 63) << 2) | 2;
    const bg = (((k >> 6) & 63) << 2) | 2;
    const bb = ((k & 63) << 2) | 2;
    let best = 0, bestD = 1e9;
    for (let i = 0; i < count; i++) {
      const dr = br - pr[i], dg = bg - pg[i], db = bb - pb[i];
      const d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
      if (d < bestD) { bestD = d; best = i; }
    }
    lut[k] = best;
    return best;
  };
}

function encode(frames, width, height, opts) {
  opts = opts || {};
  const maxColors = Math.max(2, Math.min(256, opts.maxColors || 256));
  const loop = opts.loop === undefined ? 0 : opts.loop;

  const q = quantize(frames, maxColors);
  const count = Math.max(2, q.count);
  let bits = 2;
  while ((1 << bits) < count) bits++;
  if (bits > 8) bits = 8;

  const palEntries = 1 << bits;
  const pal = new Uint8Array(palEntries * 3);
  pal.set(q.palette.subarray(0, Math.min(q.palette.length, palEntries * 3)));

  const lookup = buildLut(pal, Math.min(q.count, palEntries));
  const out = [];

  const push16 = (v) => { out.push(v & 0xFF, (v >> 8) & 0xFF); };

  const head = 'GIF89a';
  for (let i = 0; i < head.length; i++) out.push(head.charCodeAt(i));
  push16(width);
  push16(height);
  out.push(0x80 | ((bits - 1) << 4) | (bits - 1));
  out.push(0);
  out.push(0);
  for (let i = 0; i < pal.length; i++) out.push(pal[i]);

  out.push(0x21, 0xFF, 0x0B);
  const netscape = 'NETSCAPE2.0';
  for (let i = 0; i < netscape.length; i++) out.push(netscape.charCodeAt(i));
  out.push(0x03, 0x01);
  push16(loop);
  out.push(0x00);

  for (let f = 0; f < frames.length; f++) {
    const fr = frames[f];
    const d = fr.rgba;
    const idx = new Uint8Array(width * height);
    for (let i = 0, p = 0; i < idx.length; i++, p += 4) {
      idx[i] = lookup(d[p], d[p + 1], d[p + 2]);
    }

    let cs = Math.round((fr.delay || 0) / 10);
    if (cs < 2) cs = 10;
    if (cs > 65535) cs = 65535;

    out.push(0x21, 0xF9, 0x04, 0x04);
    push16(cs);
    out.push(0x00, 0x00);

    out.push(0x2C);
    push16(0); push16(0);
    push16(width); push16(height);
    out.push(0x00);

    const data = lzwEncode(bits, idx);
    out.push(bits);
    for (let i = 0; i < data.length; i += 255) {
      const n = Math.min(255, data.length - i);
      out.push(n);
      for (let k = 0; k < n; k++) out.push(data[i + k]);
    }
    out.push(0x00);
  }

  out.push(0x3B);
  return { bytes: new Uint8Array(out), palette: palEntries, frames: frames.length };
}

function mergeIdentical(frames) {
  const out = [];
  for (let i = 0; i < frames.length; i++) {
    const f = frames[i];
    const prev = out[out.length - 1];
    if (prev && sameRgba(prev.rgba, f.rgba)) {
      prev.delay += f.delay;
      if (prev.delay > 655350) prev.delay = 655350;
      continue;
    }
    out.push({ rgba: f.rgba, delay: f.delay });
  }
  return out;
}

function sameRgba(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 4) {
    if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) return false;
  }
  return true;
}

function dropHalf(frames) {
  const out = [];
  for (let i = 0; i < frames.length; i += 2) {
    const a = frames[i];
    const b = frames[i + 1];
    out.push({ rgba: a.rgba, delay: b ? a.delay + b.delay : a.delay });
  }
  return out;
}

function fitToBudget(inputFrames, width, height, maxBytes, opts) {
  let frames = mergeIdentical(inputFrames);
  let dropped = 0;
  let result = null;

  for (let attempt = 0; attempt < 8; attempt++) {
    const maxColors = attempt < 2 ? 256 : attempt < 4 ? 128 : 64;
    result = encode(frames, width, height, { maxColors: maxColors, loop: 0 });
    if (result.bytes.length <= maxBytes || frames.length === 1) break;
    if (frames.length > 1) {
      dropped += frames.length - Math.ceil(frames.length / 2);
      frames = dropHalf(frames);
    }
  }
  result.dropped = dropped;
  result.frames = frames.length;
  return result;
}

return {
  decode: decode,
  encode: encode,
  fitToBudget: fitToBudget,
  mergeIdentical: mergeIdentical,
  dropHalf: dropHalf,
  _lzwDecode: lzwDecode,
  _lzwEncode: lzwEncode
};
});

const TARGET = 64;
const UPLOAD_BUDGET = 1040000;

function makeCanvas(w, h) {
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  return c;
}
function ctx2d(cv) {
  return cv.getContext('2d', { willReadFrequently: true });
}

// Draws src (with crop applied, if any) onto ctx at 64x64 using the chosen mode
function drawFit(ctx, src, sw, sh, mode) {
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, TARGET, TARGET);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  if (!sw || !sh) return;

  let sx = 0, sy = 0, scw = sw, sch = sh;
  if (cropState.rect && cropState.rect.w > 0 && cropState.rect.h > 0) {
    const r = cropState.rect;
    sx = r.x * sw;
    sy = r.y * sh;
    scw = Math.max(1, r.w * sw);
    sch = Math.max(1, r.h * sh);
  }

  if (mode === 'fill') {
    const r = Math.max(TARGET / scw, TARGET / sch);
    const dw = scw * r, dh = sch * r;
    ctx.drawImage(src, sx, sy, scw, sch,
                  (TARGET - dw) / 2, (TARGET - dh) / 2, dw, dh);
  } else {
    const r = Math.min(TARGET / scw, TARGET / sch);
    const dw = Math.max(1, Math.round(scw * r));
    const dh = Math.max(1, Math.round(sch * r));
    ctx.drawImage(src, sx, sy, scw, sch,
                  Math.round((TARGET - dw) / 2),
                  Math.round((TARGET - dh) / 2), dw, dh);
  }
}

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => { URL.revokeObjectURL(url); resolve(img); };
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('Cannot read the image')); };
    img.src = url;
  });
}

function yieldToUi() {
  return new Promise(r => requestAnimationFrame(() => setTimeout(r, 0)));
}

function describe(res) {
  let s = `${fmtSize(res.bytes.length)}, ${res.frames} fr.`;
  if (res.palette) s += `, palette ${res.palette}`;
  if (res.dropped) s += `, dropped ${res.dropped} fr.`;
  return s;
}

async function convertGif(file) {
  const mode = $('fitMode').value;
  const fixed = customDelay();
  const buf = new Uint8Array(await file.arrayBuffer());

  let srcCanvas = null, srcCtx = null;
  const dst = makeCanvas(TARGET, TARGET);
  const dctx = ctx2d(dst);
  const frames = [];

  const info = GifCodec.decode(buf, f => {
    if (!srcCanvas || srcCanvas.width !== f.width || srcCanvas.height !== f.height) {
      srcCanvas = makeCanvas(f.width, f.height);
      srcCtx = ctx2d(srcCanvas);
    }
    srcCtx.putImageData(new ImageData(f.rgba, f.width, f.height), 0, 0);
    drawFit(dctx, srcCanvas, f.width, f.height, mode);
    const px = dctx.getImageData(0, 0, TARGET, TARGET);
    frames.push({ rgba: px.data, delay: fixed || f.delay });
  });

  if (!frames.length) throw new Error('GIF contains no frames');
  if (frames.length === 1 && !fixed) frames[0].delay = 1000;

  const res = GifCodec.fitToBudget(frames, TARGET, TARGET, UPLOAD_BUDGET);
  return {
    blob: new Blob([res.bytes], { type: 'image/gif' }),
    ext: 'gif',
    info: `GIF ${info.width}×${info.height} (${info.frames} fr.) → 64×64 ${describe(res)}` +
          (fixed ? `, ${fixed} ms/fr.` : '')
  };
}

async function convertStatic(file) {
  const mode = $('fitMode').value;
  const fixed = customDelay();
  const img = await loadImage(file);
  const sw = img.naturalWidth || img.width;
  const sh = img.naturalHeight || img.height;

  const dst = makeCanvas(TARGET, TARGET);
  const dctx = ctx2d(dst);
  drawFit(dctx, img, sw, sh, mode);

  const data = dctx.getImageData(0, 0, TARGET, TARGET).data;
  const colors = new Set();
  for (let i = 0; i < data.length; i += 4)
    colors.add((data[i] << 16) | (data[i + 1] << 8) | data[i + 2]);

  const base = `${sw}×${sh} → 64×64`;

  if (colors.size <= 256) {
    const res = GifCodec.encode([{ rgba: data, delay: fixed || 1000 }], TARGET, TARGET, { maxColors: 256 });
    return {
      blob: new Blob([res.bytes], { type: 'image/gif' }),
      ext: 'gif',
      info: `${base} GIF (${colors.size} colors, ${fmtSize(res.bytes.length)})`
    };
  }

  const blob = await new Promise(r => dst.toBlob(r, 'image/jpeg', 0.9));
  if (!blob) throw new Error('Failed to create JPEG');
  return { blob, ext: 'jpg', info: `${base} JPEG (${colors.size} colors, ${fmtSize(blob.size)})` };
}

async function getGifDimensions(file) {
  const buf = await file.slice(0, 10).arrayBuffer();
  const b = new Uint8Array(buf);
  if (b[0] !== 0x47 || b[1] !== 0x49 || b[2] !== 0x46) return null;
  return { width: b[6] | (b[7] << 8), height: b[8] | (b[9] << 8) };
}

const STATIC_EXT = ['jpg', 'jpeg', 'png', 'webp', 'bmp'];

async function processFile(file) {
  const ext = file.name.split('.').pop().toLowerCase();
  const base = (file.name.replace(/\.[^.]+$/, '') || 'image').substring(0, 40);

  if (ext !== 'gif' && !STATIC_EXT.includes(ext))
    throw new Error('Unsupported format: .' + ext);

  if (ext === 'gif') {
    const dims = await getGifDimensions(file);
    if (!dims) throw new Error('Invalid GIF file');
    if (dims.width === TARGET && dims.height === TARGET &&
        file.size <= UPLOAD_BUDGET && !customDelay() && !cropState.rect)
      return { file, info: 'GIF 64×64 - sent unchanged (animation untouched)' };
  }

  setStatus('Converting ' + file.name + '…');
  await yieldToUi();

  const conv = ext === 'gif' ? await convertGif(file) : await convertStatic(file);
  return {
    file: new File([conv.blob], base + '_64.' + conv.ext, { type: conv.blob.type }),
    info: conv.info
  };
}

async function updateFileInfo() {
  const f = $('file').files && $('file').files[0];
  if (!f) { fileInfo.textContent = ''; return; }
  fileInfo.style.color = '';
  fileInfo.textContent = `Selected: ${f.name} (${fmtSize(f.size)})`;
  try {
    const ext = f.name.split('.').pop().toLowerCase();
    if (ext === 'gif') {
      const dims = await getGifDimensions(f);
      if (!dims) throw new Error('not a GIF file');
      if (dims.width === TARGET && dims.height === TARGET &&
          f.size <= UPLOAD_BUDGET && !customDelay() && !cropState.rect)
        fileInfo.textContent = `✓ GIF 64×64 (${fmtSize(f.size)}) - ready to upload`;
      else
        fileInfo.textContent = `✓ GIF ${dims.width}×${dims.height} (${fmtSize(f.size)}) - will scale to 64×64 and keep the animation`;
      fileInfo.style.color = 'var(--accent)';
      return;
    }
    if (STATIC_EXT.includes(ext)) {
      fileInfo.textContent = `✓ ${ext.toUpperCase()} (${fmtSize(f.size)}) - will scale to 64×64 (GIF or JPEG)`;
      fileInfo.style.color = 'var(--accent)';
      return;
    }
    fileInfo.textContent = `⚠ Unknown format .${ext}`;
    fileInfo.style.color = '#f59e0b';
  } catch(e) {
    fileInfo.textContent = 'Error: ' + e.message;
    fileInfo.style.color = 'var(--danger)';
  }
}

// ============ CROP ============
// cropState.rect is normalized {x,y,w,h} in [0,1] of the source dimensions.
// drawFit() consults it on every conversion / preview render.

const cropState = {
  rect: null,
  canvas: null,
  ctx: null,
  dispW: 0,
  dispH: 0,
  sourceImg: null,       // HTMLImageElement or Canvas (first GIF frame)
  dragStart: null
};

let sourcePreviewGen = 0;

function redrawCropOverlay() {
  if (!cropState.ctx) return;
  const { ctx, dispW, dispH, sourceImg } = cropState;
  ctx.clearRect(0, 0, dispW, dispH);
  ctx.drawImage(sourceImg, 0, 0, dispW, dispH);

  if (cropState.rect) {
    const r = cropState.rect;
    const x = r.x * dispW, y = r.y * dispH;
    const w = r.w * dispW, h = r.h * dispH;

    ctx.fillStyle = 'rgba(0,0,0,0.55)';
    ctx.fillRect(0, 0, dispW, y);
    ctx.fillRect(0, y + h, dispW, dispH - y - h);
    ctx.fillRect(0, y, x, h);
    ctx.fillRect(x + w, y, dispW - x - w, h);

    ctx.strokeStyle = '#22c55e';
    ctx.lineWidth = 2;
    ctx.strokeRect(x + 1, y + 1, Math.max(1, w - 2), Math.max(1, h - 2));
  }
}

function updateCropInfo() {
  const info = $('cropInfo');
  const btn = $('clearCropBtn');
  if (!cropState.rect) {
    info.textContent = 'Full image';
    btn.style.display = 'none';
    return;
  }
  const r = cropState.rect;
  const wPct = Math.round(r.w * 100);
  const hPct = Math.round(r.h * 100);
  info.textContent = `Crop: ${wPct}% × ${hPct}%`;
  btn.style.display = 'inline-block';
}

async function setSourcePreviewFromFile(f) {
  const myGen = ++sourcePreviewGen;

  const ext = f.name.split('.').pop().toLowerCase();
  let sourceImg, sw, sh;

  try {
    if (ext === 'gif') {
      const buf = new Uint8Array(await f.arrayBuffer());
      if (myGen !== sourcePreviewGen) return;
      let first = null;
      const STOP = {};
      try {
        GifCodec.decode(buf, frame => {
          if (!first) { first = frame; throw STOP; }
        });
      } catch(e) { if (e !== STOP) throw e; }
      if (!first) return;
      sw = first.width; sh = first.height;
      const c = makeCanvas(sw, sh);
      ctx2d(c).putImageData(new ImageData(first.rgba, sw, sh), 0, 0);
      sourceImg = c;
    } else if (STATIC_EXT.includes(ext)) {
      const img = await loadImage(f);
      if (myGen !== sourcePreviewGen) return;
      sourceImg = img;
      sw = img.naturalWidth || img.width;
      sh = img.naturalHeight || img.height;
    } else return;
  } catch(e) {
    $('cropArea').style.display = 'none';
    return;
  }

  const MAX = 280;
  const scale = Math.min(MAX / sw, MAX / sh, 1);
  const dw = Math.max(1, Math.round(sw * scale));
  const dh = Math.max(1, Math.round(sh * scale));

  const cv = $('cropCanvas');
  cv.width = dw;
  cv.height = dh;
  cv.style.width = dw + 'px';
  cv.style.height = dh + 'px';

  cropState.canvas = cv;
  cropState.ctx = cv.getContext('2d', { willReadFrequently: true });
  cropState.dispW = dw;
  cropState.dispH = dh;
  cropState.sourceImg = sourceImg;

  $('cropArea').style.display = 'block';
  redrawCropOverlay();
  updateCropInfo();
}

function clearCropRect() {
  cropState.rect = null;
  redrawCropOverlay();
  updateCropInfo();
  updatePreview();
}

function getNormXY(e) {
  const cv = cropState.canvas;
  const rect = cv.getBoundingClientRect();
  return {
    x: Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width)),
    y: Math.max(0, Math.min(1, (e.clientY - rect.top) / rect.height))
  };
}

// While dragging, draw the current selection to the 64x64 preview
// so the user sees the crop effect in real time.
function previewLiveCrop() {
  const cv = $('preview');
  if (!cv || !cropState.sourceImg) return;
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  const sw = cropState.sourceImg.width || cropState.sourceImg.naturalWidth;
  const sh = cropState.sourceImg.height || cropState.sourceImg.naturalHeight;
  drawFit(ctx, cropState.sourceImg, sw, sh, $('fitMode').value);
}

function onCropPointerDown(e) {
  if (!cropState.canvas) return;
  e.preventDefault();
  stopPreviewAnimation();
  const p = getNormXY(e);
  cropState.dragStart = p;
  cropState.rect = { x: p.x, y: p.y, w: 0, h: 0 };
  if (cropState.canvas.setPointerCapture) {
    try { cropState.canvas.setPointerCapture(e.pointerId); } catch(_) {}
  }
}

function onCropPointerMove(e) {
  if (!cropState.dragStart) return;
  e.preventDefault();
  const cur = getNormXY(e);
  const s = cropState.dragStart;
  cropState.rect = {
    x: Math.min(s.x, cur.x),
    y: Math.min(s.y, cur.y),
    w: Math.abs(cur.x - s.x),
    h: Math.abs(cur.y - s.y)
  };
  redrawCropOverlay();
  previewLiveCrop();
}

function onCropPointerUp(e) {
  if (!cropState.dragStart) return;
  cropState.dragStart = null;

  // Discard tiny crops (accidental clicks)
  if (cropState.rect && (cropState.rect.w < 0.02 || cropState.rect.h < 0.02)) {
    cropState.rect = null;
    redrawCropOverlay();
    updateCropInfo();
    updatePreview();
    return;
  }
  updateCropInfo();
  updatePreview();
}

// ============ PREVIEW ============
// Live 64x64 animated preview.
//   - GIF: all frames decoded once, downscaled (crop applied), identical-merged,
//          then looped with setTimeout using the effective delay.
//   - Static image: drawn once (no loop).
// Delay slider change re-schedules the next frame instantly (debounced).
// fitMode change re-decodes. Crop change re-decodes.

const previewState = {
  generation: 0,
  timerId: null,
  frames: null,       // [{ imageData, delay }]  -- 64x64 already scaled
  index: 0
};

function stopPreviewAnimation() {
  if (previewState.timerId) {
    clearTimeout(previewState.timerId);
    previewState.timerId = null;
  }
  previewState.frames = null;
  previewState.index = 0;
}

function sameRgbaData(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 4) {
    if (a[i] !== b[i] || a[i+1] !== b[i+1] || a[i+2] !== b[i+2]) return false;
  }
  return true;
}

function animatePreviewFrame(ctx, cv, gen) {
  if (gen !== previewState.generation) return;
  if (!previewState.frames || !previewState.frames.length) return;

  const frame = previewState.frames[previewState.index];
  ctx.putImageData(frame.imageData, 0, 0);

  if (previewState.frames.length <= 1) {
    previewState.timerId = null;
    return;
  }

  const fixed = customDelay();
  let d = fixed || frame.delay;
  if (d < 20) d = 100;

  previewState.index = (previewState.index + 1) % previewState.frames.length;
  previewState.timerId = setTimeout(
    () => animatePreviewFrame(ctx, cv, gen), d);
}

// Delay slider changed -> reschedule the next frame with the new delay
// without restarting the whole animation.
function reschedulePreviewWithNewDelay() {
  if (!previewState.frames || previewState.frames.length <= 1) return;
  const cv = $('preview');
  if (!cv || cv.style.display === 'none') return;
  if (previewState.timerId) {
    clearTimeout(previewState.timerId);
    previewState.timerId = null;
  }
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  if (previewState.index > 0) previewState.index--;
  else previewState.index = 0;
  animatePreviewFrame(ctx, cv, previewState.generation);
}

async function updatePreview() {
  const cv = $('preview');
  const label = $('previewLabel');
  if (!cv) return;
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  const gen = ++previewState.generation;

  stopPreviewAnimation();
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, cv.width, cv.height);

  const f = $('file').files && $('file').files[0];
  if (!f) {
    cv.style.display = 'none';
    if (label) label.style.display = 'none';
    return;
  }

  cv.style.display = 'block';
  if (label) label.style.display = 'block';
  const mode = $('fitMode').value;

  try {
    const ext = f.name.split('.').pop().toLowerCase();

    if (ext === 'gif') {
      const buf = new Uint8Array(await f.arrayBuffer());
      if (gen !== previewState.generation) return;

      const tmp = makeCanvas(TARGET, TARGET);
      const tctx = ctx2d(tmp);
      const rawFrames = [];

      GifCodec.decode(buf, frame => {
        const src = makeCanvas(frame.width, frame.height);
        ctx2d(src).putImageData(new ImageData(frame.rgba, frame.width, frame.height), 0, 0);
        drawFit(tctx, src, frame.width, frame.height, mode);
        rawFrames.push({
          imageData: tctx.getImageData(0, 0, TARGET, TARGET),
          delay: frame.delay
        });
      });

      if (gen !== previewState.generation) return;
      if (!rawFrames.length) { cv.style.display = 'none'; return; }

      const merged = [];
      for (let i = 0; i < rawFrames.length; i++) {
        const cur = rawFrames[i];
        const prev = merged[merged.length - 1];
        if (prev && sameRgbaData(prev.imageData.data, cur.imageData.data)) {
          prev.delay += cur.delay;
        } else {
          merged.push(cur);
        }
      }

      previewState.frames = merged;
      previewState.index = 0;
      animatePreviewFrame(ctx, cv, gen);

    } else if (STATIC_EXT.includes(ext)) {
      const img = await loadImage(f);
      if (gen !== previewState.generation) return;
      const sw = img.naturalWidth || img.width;
      const sh = img.naturalHeight || img.height;
      drawFit(ctx, img, sw, sh, mode);
    }
  } catch(e) {
    // Silent — updateFileInfo already surfaces format errors.
  }
}

// ============ FILE CHANGE ============
async function onFileChange() {
  cropState.rect = null;
  const f = $('file').files && $('file').files[0];

  if (!f) {
    $('cropArea').style.display = 'none';
    fileInfo.textContent = '';
    updatePreview();
    return;
  }

  updateFileInfo();
  try {
    await setSourcePreviewFromFile(f);
  } catch(_) {
    $('cropArea').style.display = 'none';
  }
  updatePreview();
}

$('file').onchange = onFileChange;
$('fitMode').onchange = () => { updatePreview(); };
$('clearCropBtn').onclick = clearCropRect;

// Bind crop canvas pointer events
(function bindCropEvents() {
  const cv = $('cropCanvas');
  if (!cv) return;
  cv.addEventListener('pointerdown', onCropPointerDown);
  cv.addEventListener('pointermove', onCropPointerMove);
  cv.addEventListener('pointerup', onCropPointerUp);
  cv.addEventListener('pointercancel', onCropPointerUp);
})();

// ============ UPLOAD ============
function uploadOne(file) {
  return new Promise((resolve, reject) => {
    const fd = new FormData();
    fd.append('file', file);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload');
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        const p = (e.loaded / e.total * 100).toFixed(0);
        bar.style.width = p + '%';
        setStatus('Uploading ' + file.name + '… ' + p + '%');
      }
    };
    xhr.onload = () => {
      if (xhr.status === 401) showLogin();
      if (xhr.status === 200) resolve();
      else reject(new Error(xhr.responseText || ('HTTP ' + xhr.status)));
    };
    xhr.onerror = () => reject(new Error('network error'));
    xhr.send(fd);
  });
}

$('uploadBtn').onclick = async () => {
  const list = Array.from($('file').files || []);
  if (!list.length) { setStatus('Select a file.', 'error'); return; }

  $('uploadBtn').disabled = true;
  stopPreviewAnimation();   // free CPU during conversion/upload
  prog.style.display = 'block';

  let ok = 0, failed = 0;
  for (let i = 0; i < list.length; i++) {
    const f = list[i];
    const tag = list.length > 1 ? ` (${i + 1}/${list.length})` : '';
    try {
      setStatus('Processing ' + f.name + tag + '…');
      const res = await processFile(f);
      bar.style.width = '0%';
      await uploadOne(res.file);
      ok++;
      setStatus('✓ ' + res.info + tag, 'ok');
    } catch(e) {
      failed++;
      setStatus('✗ ' + f.name + ': ' + e.message, 'error');
      await new Promise(r => setTimeout(r, 1200));
    }
  }

  prog.style.display = 'none';
  $('uploadBtn').disabled = false;
  $('file').value = '';
  cropState.rect = null;
  cropState.sourceImg = null;
  $('cropArea').style.display = 'none';
  fileInfo.textContent = '';
  updatePreview();
  refresh();
  if (failed) setStatus(`Done: ${ok} uploaded, ${failed} failed`, 'error');
  else if (ok > 1) setStatus(`✓ ${ok} files uploaded and queued`, 'ok');
};

// ============ STATUS / DEVICE ============
function fmtUptime(s) {
  s = s | 0;
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
  if (s < 86400) return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm';
  return Math.floor(s / 86400) + 'd ' + Math.floor((s % 86400) / 3600) + 'h';
}

function applyStatus(s) {
  const playing = s.file ? s.file.replace(/^\//, '') : '';
  if (s.type === 'clock') {
    $('stFile').textContent = '🕒 clock';
    $('stFile').title = '';
  } else if (s.type === 'qr') {
    $('stFile').textContent = '📱 QR';
    $('stFile').title = '';
  } else {
    $('stFile').textContent = playing ? (s.paused ? '⏸ ' : '▶ ') + playing : 'idle';
    $('stFile').title = playing;
  }
  $('stPl').textContent = (s.files && s.index) ? s.index + '/' + s.files : '—';
  $('stFps').textContent = s.type === 'gif'
    ? (s.paused ? 'paused' : s.fps + ' fps')
    : s.type === 'jpeg' ? 'static'
    : s.type === 'clock' ? 'time' : '-'
  const fsPct = s.fsTotal ? Math.round(s.fsUsed / s.fsTotal * 100) : 0;
  $('stFs').textContent = fsPct + '% of ' + fmtSize(s.fsTotal);
  $('stFs').title = fmtSize(s.fsUsed) + ' / ' + fmtSize(s.fsTotal);
  $('fsBar').style.width = fsPct + '%';
  $('stHeap').textContent = Math.round(s.heap / 1024) + ' KB';
  $('stRssi').textContent = s.ap ? 'AP mode' : (s.rssi ? s.rssi + ' dBm' : '—');
  $('stUp').textContent = fmtUptime(s.uptime);
  $('wifiInfo').textContent = s.ap
    ? `Fallback AP: ${s.apSsid} · http://${s.apIp} · pass: ledmatrix`
    : `Wi-Fi: ${s.ip || 'not connected'}`;
  $('pauseBtn').textContent = s.paused ? '▶ Resume' : '⏸ Pause';
  $('autoRot').checked = !!s.auto;
  $('shuffle').checked = !!s.shuffle;
  if (document.activeElement !== $('rotateSec')) $('rotateSec').value = s.rotate;
  if (document.activeElement !== bright) {
    bright.value = s.brightness;
    brightVal.textContent = s.brightness;
  }
  if (!panelInit && s.pwr !== undefined) {
    panelInit = true;
    $('clkFace').value = String(s.clockFace ?? 0);
    $('clkDate').checked = !!s.clockDate;
    syncTz(s.tz);
    $('schOn').checked = !!s.sched;
    $('schFrom').value = minToTime(s.schedOn ?? 480);
    $('schTo').value = minToTime(s.schedOff ?? 1380);
    $('pwrLimit').value = s.pwr ?? 100;
    $('pwrVal').textContent = (s.pwr ?? 100) + '%';
    $('fadeTog').checked = !!s.fade;
    $('rotSel').value = String(s.rot ?? 0);
  }
  if (s.pin !== undefined) {
    $('pinState').textContent = s.pin
      ? 'PIN protection is on.'
      : 'No PIN set — anyone on the network can control the panel.';
  }
}

// "HH:MM" (UTC) helpers for the schedule and time-zone controls.
function minToTime(m) {
  m = Math.max(0, Math.min(1439, m | 0));
  return String(Math.floor(m / 60)).padStart(2, '0') + ':' +
         String(m % 60).padStart(2, '0');
}

function timeToMin(v) {
  const parts = (v || '').split(':');
  if (parts.length !== 2) return null;
  const h = parseInt(parts[0], 10), m = parseInt(parts[1], 10);
  if (isNaN(h) || isNaN(m) || h < 0 || h > 23 || m < 0 || m > 59) return null;
  return h * 60 + m;
}

function syncTz(tz) {
  if (!tz) return;
  const sel = $('clkTz');
  const found = Array.from(sel.options).some(o => o.value === tz);
  if (found) { sel.value = tz; return; }
  const o = document.createElement('option');
  o.value = tz;
  o.textContent = tz;
  sel.appendChild(o);
  sel.value = tz;
}

// ============ FILE LIST ============
async function refresh() {
  try {
    const [filesRes, statusRes] = await Promise.all([
      fetch('/files-json'), fetch('/status')
    ]);
    const files = await filesRes.json();
    const status = await statusRes.json();

    applyStatus(status);

    const div = $('files');
    if (files.length === 0) {
      div.innerHTML = '<div class="empty">no files - upload something above</div>';
      return;
    }
    div.innerHTML = files.map(f => {
      const on = (status.type === 'gif' || status.type === 'jpeg') &&
                 status.file === '/' + f.name;
      return `<div class="file${f.en ? '' : ' off'}" data-name="${f.name.replace(/"/g, '&quot;')}">
        <span class="name${on ? ' playing' : ''}">${on ? '▶ ' : ''}${f.name}
          <span class="size">${fmtSize(f.size)}</span></span>
        <input class="dur" type="number" min="0" max="3600" step="5" value="${f.dur}"
               data-act="dur" title="Hold seconds (0 = global)">
        <button class="ghost" data-act="up" title="Move up">↑</button>
        <button class="ghost" data-act="down" title="Move down">↓</button>
        <label class="toggle" title="Include in rotation"><input type="checkbox"
               data-act="en" ${f.en ? 'checked' : ''}></label>
        <button class="secondary" data-act="play" title="Play">▶</button>
        <button class="ghost" data-act="ren" title="Rename">✎</button>
        <button class="danger" data-act="del" title="Delete">✕</button>
      </div>`;
    }).join('');

  } catch(e) {
    setStatus('Error: ' + e.message, 'error');
  }
}

async function play(name) {
  setStatus('Playing ' + name + '...');
  const r = await fetch('/play?f=' + encodeURIComponent(name));
  if (r.ok) setStatus('▶ ' + name, 'ok');
  else setStatus('✗ Could not play ' + name, 'error');
  refresh();
}

async function del(name) {
  if (!confirm('Delete ' + name + '?')) return;
  await fetch('/del?f=' + encodeURIComponent(name));
  setStatus('Deleted ' + name);
  refresh();
}

$('files').onclick = async e => {
  const b = e.target.closest('[data-act]');
  if (!b) return;
  const row = b.closest('.file');
  const n = row && row.dataset.name;
  if (!n) return;
  const act = b.dataset.act;
  try {
    if (act === 'play') return play(n);
    if (act === 'del') return del(n);
    if (act === 'ren') {
      const to = prompt('New name', n);
      if (!to || to === n) return;
      const r = await fetch('/rename?f=' + encodeURIComponent(n) + '&to=' + encodeURIComponent(to));
      if (!r.ok) setStatus('✗ ' + await r.text(), 'error');
      return refresh();
    }
    if (act === 'up' || act === 'down') {
      await fetch('/pl?f=' + encodeURIComponent(n) + '&' + act + '=1');
      return refresh();
    }
  } catch(e) { setStatus('Error: ' + e.message, 'error'); }
};

$('files').onchange = async e => {
  const act = e.target.dataset.act;
  if (act !== 'en' && act !== 'dur') return;
  const row = e.target.closest('.file');
  if (!row) return;
  const n = row.dataset.name;
  const q = act === 'en' ? '&en=' + (e.target.checked ? 1 : 0)
                         : '&dur=' + (parseInt(e.target.value, 10) || 0);
  await fetch('/pl?f=' + encodeURIComponent(n) + q);
  refresh();
};

$('delAll').onclick = async () => {
  if (!confirm('Delete every file on the device? This cannot be undone.')) return;
  await fetch('/delall');
  setStatus('All files deleted.');
  refresh();
};

async function stop() {
  await fetch('/stop');
  setStatus('Stopped.');
  refresh();
}

// ============ PLAYBACK CONTROLS ============
async function ctl(q, msg) {
  try {
    const r = await fetch('/ctl?' + q);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    if (msg) setStatus(msg, 'ok');
    refresh();
  } catch(e) {
    setStatus('Error: ' + e.message, 'error');
  }
}

$('prevBtn').onclick = () => ctl('a=prev', '⏮ Previous file');
$('nextBtn').onclick = () => ctl('a=next', '⏭ Next file');
$('pauseBtn').onclick = () => ctl('a=toggle');
$('autoRot').onchange = () => ctl(
  'a=auto&v=' + ($('autoRot').checked ? 1 : 0),
  $('autoRot').checked ? 'Auto-rotate on' : 'Auto-rotate off');
$('shuffle').onchange = () => ctl(
  'a=shuffle&v=' + ($('shuffle').checked ? 1 : 0),
  $('shuffle').checked ? 'Shuffle on' : 'Shuffle off');
$('rotateSec').onchange = () => {
  let v = parseInt($('rotateSec').value, 10);
  if (!v || v < 5) v = 5;
  if (v > 3600) v = 3600;
  $('rotateSec').value = v;
  ctl('a=interval&v=' + v, 'Rotate every ' + v + ' s');
};

// ============ WIFI SETUP ============
$('wifiSave').onclick = async () => {
  const ssid = $('wifiSsid').value.trim();
  if (!ssid) { setStatus('Enter the network name first.', 'error'); return; }
  setStatus('Saving Wi-Fi, the device will reboot…');
  try {
    await fetch('/wifi?ssid=' + encodeURIComponent(ssid) +
                '&pass=' + encodeURIComponent($('wifiPass').value));
    $('wifiPass').value = '';
    setStatus('Saved. Reconnecting to ' + ssid + '…', 'ok');
  } catch(e) {
    setStatus('Error: ' + e.message, 'error');
  }
};

// ============ CLOCK / SCHEDULE / PANEL ============
async function api(q, msg) {
  try {
    const r = await fetch(q);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    if (msg) setStatus(msg, 'ok');
    refresh();
  } catch(e) {
    setStatus('Error: ' + e.message, 'error');
  }
}


$('clkStart').onclick = () => api(
  '/clock?face=' + $('clkFace').value +
  '&date=' + ($('clkDate').checked ? 1 : 0) +
  '&tz=' + encodeURIComponent($('clkTz').value),
  '🕒 Clock shown.');
$('clkStop').onclick = () => api('/clock?off=1', 'Clock stopped.');

function schQuery() {
  const from = $('schFrom').value, to = $('schTo').value;
  return 'on=' + ($('schOn').checked ? 1 : 0) +
         '&from=' + encodeURIComponent(from) +
         '&to=' + encodeURIComponent(to);
}

$('schSave').onclick = () => api('/schedule?' + schQuery(), 'Schedule saved.');
$('schOn').onchange = () => api('/schedule?' + schQuery(),
  $('schOn').checked ? 'Schedule enabled.' : 'Schedule disabled.');

const pwrLimit = $('pwrLimit'), pwrVal = $('pwrVal');
let pwrTimer;
pwrLimit.oninput = () => {
  pwrVal.textContent = pwrLimit.value + '%';
  clearTimeout(pwrTimer);
  pwrTimer = setTimeout(() => fetch('/panel?pwr=' + pwrLimit.value), 100);
};
$('fadeTog').onchange = () => api('/panel?fade=' + ($('fadeTog').checked ? 1 : 0),
  $('fadeTog').checked ? 'Fades on.' : 'Fades off.');
$('rotSel').onchange = () => api('/panel?rot=' + $('rotSel').value,
  'Rotation ' + ($('rotSel').value * 90) + ' degrees');

$('qrBtn').onclick = async () => {
  const r = await fetch('/qr');
  if (r.ok) setStatus('📱 QR on the panel points at ' + (await r.text()), 'ok');
};
$('qrBoot').onclick = async () => {
  const on = !($('qrBoot').dataset.on === '1');
  await fetch('/panel?qr=' + (on ? 1 : 0));
  $('qrBoot').dataset.on = on ? '1' : '0';
  $('qrBoot').textContent = 'QR on boot: ' + (on ? 'on' : 'off');
  setStatus('QR at boot ' + (on ? 'enabled' : 'disabled') + '.', 'ok');
};
$('cfgBtn').onclick = async () => {
  const t = await (await fetch('/config')).text();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([t], {type: 'application/json'}));
  a.download = 'ledmatrix-config.json';
  a.click();
  URL.revokeObjectURL(a.href);
  setStatus('Configuration downloaded.', 'ok');
};
$('resetBtn').onclick = async () => {
  if (!confirm('Erase all settings (not the media files) and reboot?')) return;
  await fetch('/reset');
  setStatus('Settings erased, rebooting…');
};

// ============ DIAGNOSTICS ============
$('diagBtn').onclick = async () => {
  const out = $('diagOut');
  try {
    const r = await fetch('/diag', {cache: 'no-store'});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    out.textContent = Object.keys(j).map(k => k.padEnd(11) + ' ' + j[k]).join('\n');
    out.style.display = out.style.display === 'none' ? 'block' : 'none';
  } catch (e) {
    setStatus('Diagnostics unavailable: ' + e.message, 'error');
  }
};

// ============ PIN ============
$('pinSave').onclick = async () => {
  const p = $('pinSet').value.trim();
  if (p.length < 4) { setStatus('Use at least 4 characters.', 'error'); return; }
  const r = await fetch('/pin?set=' + encodeURIComponent(p));
  if (r.ok) {
    $('pinSet').value = '';
    setStatus('PIN set. You are still signed in on this device.', 'ok');
    refresh();
  } else setStatus('Could not set the PIN.', 'error');
};
$('pinClear').onclick = async () => {
  if (!confirm('Remove PIN protection?')) return;
  await fetch('/pin?off=1');
  setStatus('PIN protection removed.', 'ok');
  refresh();
};

// ============ BRIGHTNESS ============
const bright = $('bright'), brightVal = $('brightVal');
let brightTimer;
bright.oninput = () => {
  brightVal.textContent = bright.value;
  clearTimeout(brightTimer);
  brightTimer = setTimeout(() => fetch('/bright?v=' + bright.value), 80);
};

refresh();
startWs();
setInterval(() => { if (!wsOk) refresh(); }, 5000);
setInterval(() => { if (wsOk) refresh(); }, 20000);
</script></body></html>
)rawliteral";

// ==================== SETUP ====================
const char *resetReasonName(uint8_t r) {
  switch (r) {
    case 1:  return "power-on";
    case 2:  return "external";
    case 3:  return "software";
    case 4:  return "panic";
    case 5:  return "int-wdt";
    case 6:  return "task-wdt";
    case 7:  return "wdt";
    case 8:  return "deepsleep";
    case 9:  return "brownout";
    case 10: return "sdio";
    case 11: return "usb";
    case 12: return "jtag";
    case 13: return "efuse";
    case 14: return "pwr-glitch";
    case 15: return "cpu-lockup";
    default: return "unknown";
  }
}

// Records a setup() milestone and prints the current memory picture.
void setBootStage(uint8_t s) {
  bootStage = s;
  uint32_t h = ESP.getFreeHeap();
  if (h < minFreeHeap) minFreeHeap = h;
  Serial.printf("[BOOT] stage %-2u heap=%u largest=%u\n", (unsigned)s, (unsigned)h,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// If setup() ever stalls (e.g. a driver waiting on DMA), restart instead of hanging forever.
void bootGuardCb(void *) {
  Serial.println("[BOOT] setup() stalled - restarting");
  Serial.flush();
  ESP.restart();
}

void armBootGuard(uint32_t ms) {
  if (bootGuardTimer) return;
  const esp_timer_create_args_t args = {
    .callback = &bootGuardCb,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "bootguard",
    .skip_unhandled_events = false,
  };
  if (esp_timer_create(&args, &bootGuardTimer) == ESP_OK)
    esp_timer_start_once(bootGuardTimer, (uint64_t)ms * 1000ULL);
}

void disarmBootGuard() {
  if (!bootGuardTimer) return;
  esp_timer_stop(bootGuardTimer);
  esp_timer_delete(bootGuardTimer);
  bootGuardTimer = nullptr;
}

void reserveHeap() {
  heapReserve = malloc(HEAP_RESERVE_BYTES);
  Serial.printf("[MEM] reserve %u B -> %s (heap=%u largest=%u)\n",
                (unsigned)HEAP_RESERVE_BYTES, heapReserve ? "ok" : "FAILED",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void releaseHeap() {
  if (!heapReserve) return;
  free(heapReserve);
  heapReserve = nullptr;
  Serial.printf("[MEM] released (heap=%u largest=%u)\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// A restart we asked for must not count towards the crash-loop guard.
void restartNow() {
  bootCount = 0;
  bootMarker = 0;
  delay(50);
  ESP.restart();
}

const char *currentModeName() {
  if (showingClock) return "clock";
  if (showingQr) return "qr";
  if (playingGif) return "gif";
  if (showingJpeg) return "jpeg";
  return "none";
}

// Everything needed to diagnose "the page does not open" without a serial cable.
String buildDiagJson() {
  PlaybackLock guard;
  bool sta = WiFi.status() == WL_CONNECTED;
  String j = "{";
  j += "\"version\":\"" FW_VERSION "\"";
  j += ",\"uptime\":" + String((unsigned long)(millis() - bootStartedAt) / 1000UL);
  j += ",\"stage\":" + String((unsigned)bootStage);
  j += ",\"boots\":" + String((unsigned)bootCount);
  j += ",\"safe\":" + String(safeMode ? "true" : "false");
  j += ",\"reset\":\"" + String(resetReasonName(bootResetReason)) + "\"";
  j += ",\"heap\":" + String((unsigned)ESP.getFreeHeap());
  j += ",\"heapMin\":" + String((unsigned)minFreeHeap);
  j += ",\"heapLargest\":" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  j += ",\"heapTotal\":" + String((unsigned)ESP.getHeapSize());
  j += ",\"display\":" + String(displayOk ? "true" : "false");
  j += ",\"doubleBuff\":" + String((displayOk && dma_display->getCfg().double_buff) ? "true" : "false");
  j += ",\"asyncTcp\":" + String(xTaskGetHandle("async_tcp") ? "true" : "false");
  j += ",\"wifi\":" + String((int)WiFi.status());
  j += ",\"ap\":" + String(apFallback ? "true" : "false");
  j += ",\"ssid\":\"" + String(sta ? WiFi.SSID() : apSsid) + "\"";
  j += ",\"ip\":\"" + String(sta ? WiFi.localIP().toString()
                                 : WiFi.softAPIP().toString()) + "\"";
  j += ",\"rssi\":" + String(sta ? WiFi.RSSI() : 0);
  j += ",\"host\":\"ledmatrix.local\"";
  j += ",\"fsUsed\":" + String((unsigned)LittleFS.usedBytes());
  j += ",\"fsTotal\":" + String((unsigned)LittleFS.totalBytes());
  j += ",\"files\":" + String((unsigned)scanFiles().size());
  j += ",\"mode\":\"" + String(currentModeName()) + "\"";
  j += ",\"file\":\"" + jsonEscape(stripSlash(currentFile)) + "\"";
  j += ",\"fps\":" + String((unsigned)currentFps);
  j += ",\"brightness\":" + String((unsigned)panelBrightness);
  j += ",\"pin\":" + String(adminPin.length() ? "true" : "false");
  j += "}";
  return j;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n========== LED MATRIX WEB PLAYER v" FW_VERSION " ==========");

  // ---- crash-loop guard: 3 fast restarts in a row -> safe mode ----
  bootStartedAt = millis();
  bootResetReason = (uint8_t)esp_reset_reason();
  bool wasMarked = (bootMarker == BOOT_MARKER_MAGIC);
  uint32_t prevStage = wasMarked ? bootStage : 0;
  if (wasMarked && bootCount < 100) {
    bootCount++;
  } else {
    bootMarker = BOOT_MARKER_MAGIC;
    bootCount = 1;
  }
  bootStage = 0;
  safeMode = bootCount >= 3;
  if (bootCount > 1)
    Serial.printf("[BOOT] previous boot restarted at stage %u\n", (unsigned)prevStage);
  Serial.printf("[BOOT] reset=%s boots=%u heap=%u largest=%u\n",
                resetReasonName(bootResetReason), (unsigned)bootCount,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (safeMode) Serial.println("[BOOT] SAFE MODE: no boot QR, no auto-resume");
  armBootGuard(60000);   // setup must finish within 60 s or the board restarts
  reserveHeap();         // hold a big contiguous block until server.begin()

  HUB75_I2S_CFG::i2s_pins _pins = {
    R1_PIN, G1_PIN, B1_PIN,
    R2_PIN, G2_PIN, B2_PIN,
    A_PIN,  B_PIN,  C_PIN, D_PIN, E_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };
  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1, _pins);
  mxconfig.clkphase = false;
  // Single framebuffer on purpose. A second 64x64 framebuffer costs ~40 KB of
  // internal RAM and fragments the heap into pieces too small for
  // AsyncWebServer's 2.9 KB send buffer (the page then sends headers but no
  // body and the browser hangs). One buffer still plays GIF/video fine.
  mxconfig.double_buff = false;
  // Full 8-bit colour depth: 256 levels per channel, no visible banding on
  // gradients. It costs ~13 KB more framebuffer RAM than 6-bit; text mode and
  // the ambient effects were removed to pay for it.
  mxconfig.setPixelColorDepthBits(8);

  // ---- restored settings (NVS) ----
  playLock = xSemaphoreCreateRecursiveMutex();
  prefs.begin("ledmatrix", false);
  currentBrightness = prefs.getUChar("bright", 90);
  if (currentBrightness < 5) currentBrightness = 5;
  autoRotate = prefs.getBool("auto", false);
  shuffleMode = prefs.getBool("shuffle", false);
  rotateSeconds = prefs.getUShort("rotate", DEFAULT_ROTATE_SEC);
  if (rotateSeconds < 5) rotateSeconds = 5;
  if (rotateSeconds > 3600) rotateSeconds = 3600;
  wifiSsid = prefs.getString("ssid", SSID);
  wifiPass = prefs.getString("pass", PASS);

  powerLimit = prefs.getUChar("pwr", 100);
  if (powerLimit < 20) powerLimit = 20;
  if (powerLimit > 100) powerLimit = 100;
  fadeEnabled = prefs.getBool("fade", true);
  panelRotation = prefs.getUChar("rot", 0);
  if (panelRotation > 3) panelRotation = 0;
  clockFace = prefs.getUChar("clkF", 0);
  if (clockFace > 1) clockFace = 0;
  clockDate = prefs.getBool("clkD", true);
  timeZone = prefs.getString("tz", "UTC0");
  schedEnabled = prefs.getBool("sch", false);
  schedOnMin = prefs.getUShort("schOn", 8 * 60);
  schedOffMin = prefs.getUShort("schOff", 23 * 60);
  adminPin = prefs.getString("pin", "");
  if (adminPin.length() > 16) adminPin = adminPin.substring(0, 16);
  scanQrAtBoot = prefs.getBool("qr", true);


  setBootStage(10);
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (safeMode && prevStage == 10) {
    // The panel driver hung inside begin() last time: stay off the bus so the UI always starts.
    Serial.println("[DISP] safe mode: skipping panel init (previous boot hung here)");
  } else {
    displayOk = dma_display->begin();
    if (!displayOk) {
      Serial.println("[DISP] begin() failed - continuing without the panel");
    }
  }
  if (!displayOk) {
    Serial.println("[DISP] panel unavailable - continuing so the web UI still starts");
  } else {
    Serial.printf("[DISP] panel ok, double_buff=%d depth=8 rot=%u\n",
                  dma_display->getCfg().double_buff ? 1 : 0,
                  (unsigned)panelRotation);
    // Rotation is applied inside the driver's drawPixel/fillRect transform, so
    // it has to be set before the first frame is drawn.
    dma_display->setRotation(panelRotation);
    presentFrame();              // align double-buffer index after begin()
    applyBrightness();
    dma_display->clearScreen();
  }

  char tok[20];
  snprintf(tok, sizeof(tok), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
  authToken = tok;

  setBootStage(20);
  // No full-screen colour flashes: they spike current on a marginal 5 V supply
  // and can brown out the board before the web server is even started.

  gif.begin(LITTLE_ENDIAN_PIXELS);
  setBootStage(30);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] MOUNT FAILED");
    if (displayOk) { dma_display->fillScreen(dma_display->color565(120, 0, 0)); presentFrame(); }
    return;
  }
  Serial.printf("[FS] %u / %u B\n", LittleFS.usedBytes(), LittleFS.totalBytes());
  loadPlaylist();
  syncPlaylist();

  // ==================== ROUTES ====================
  // Stream the UI straight out of flash; nothing is copied into RAM. Only one
  // page transfer may run at a time: each response costs a 2.9 KB send buffer
  // plus the bytes in flight in lwIP (~9 KB). A few tabs at once can drain the
  // heap, and once it is dry even the request parser's small String
  // allocations fail (ESPAsyncWebServer then dereferences a null and resets).
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (ESP.getFreeHeap() < 16000 || largest < 3000) {
      // Never close the client from inside a handler (the request would be
      // destroyed under us); abort() is the library's own way to drop one.
      r->abort();
      return;
    }
    uint32_t nowMs = millis();
    if (pageSending && nowMs - pageSendStarted < 5000) {
      // Another tab is pulling the page. Ask the browser to come back in a
      // second instead of showing a dead page or queueing 70 KB of TCP data.
      Serial.println("[WEB] page busy -> retry");
      r->send(503, "text/html",
              "<meta http-equiv=refresh content=1>"
              "<body style='font:16px system-ui;background:#111;color:#ddd'>"
              "Busy, retrying&hellip;</body>");
      return;
    }
    pageSending = true;
    pageSendStarted = nowMs;
    Serial.printf("[WEB] GET / heap=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    AsyncWebServerResponse *page = r->beginResponse("text/html", sizeof(INDEX_HTML) - 1,
      [](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        const size_t total = sizeof(INDEX_HTML) - 1;
        if (index >= total) { pageSending = false; return 0; }
        size_t n = total - index;
        if (n > maxLen) n = maxLen;
        memcpy_P(buf, INDEX_HTML + index, n);
        if (index + n >= total) pageSending = false;   // last chunk handed over
        else pageSendStarted = millis();               // keep the window fresh
        return n;
      });
    page->addHeader("Cache-Control", "no-cache");
    r->send(page);
  });

  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      if (!uploadPath.length() || uploadBytes == 0) {
        String msg = uploadError.length() ? uploadError : String("No file");
        r->send(400, "text/plain", msg);
        uploadError = "";
        return;
      }
      Serial.printf("[UPLOAD] done '%s' %u B\n", uploadPath.c_str(), uploadBytes);
      syncPlaylist();
      bool ok = playWithFade(uploadPath);
      uploadError = "";
      r->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Play failed");
    },
    [](AsyncWebServerRequest *r, String fname, size_t idx,
       uint8_t *data, size_t len, bool final) {
      uploadLastAt = millis();
      if (!idx) {
        // A previous upload that never sent its final chunk left a handle and
        // a half-written file behind. Reclaim both before starting again.
        if (uploadFile) {
          uploadFile.close();
          if (uploadPath.length()) LittleFS.remove(uploadPath);
        }
        String safe = sanitizeFilename(fname);
        uploadPath = "/" + safe;
        uploadBytes = 0;
        uploadError = "";
        if (!authed(r)) {
          uploadError = "PIN required";
          uploadPath = "";
          return;
        }

        // Replacing the file that is currently playing: close it first.
        if (currentFile == uploadPath) stopPlayback(false, false);

        // Pre-flight: never fill the filesystem completely.
        size_t existing = 0;
        if (LittleFS.exists(uploadPath)) {
          File e = LittleFS.open(uploadPath, "r");
          if (e) { existing = e.size(); e.close(); }
        }
        size_t available = LittleFS.totalBytes() - LittleFS.usedBytes() + existing;
        size_t need = r->contentLength() ? (size_t)r->contentLength()
                                         : (size_t)MAX_UPLOAD_SIZE;
        if (need + MIN_FREE_BYTES > available) {
          uploadError = "Not enough free flash space";
          uploadPath = "";
          Serial.printf("[UPLOAD] rejected: need %u B, available %u B\n",
                        (unsigned)need, (unsigned)available);
          return;
        }

        uploadFile = LittleFS.open(uploadPath, "w");
        if (!uploadFile) {
          uploadError = "Cannot create file";
          Serial.printf("[UPLOAD] cannot open '%s'\n", uploadPath.c_str());
          uploadPath = "";
          return;
        }
        Serial.printf("[UPLOAD] start '%s' (free %u B)\n",
                      uploadPath.c_str(), (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
      }
      if (len && uploadFile) {
        if (uploadBytes + len > MAX_UPLOAD_SIZE) {
          Serial.println("[UPLOAD] MAX_UPLOAD_SIZE exceeded");
          uploadFile.close();
          LittleFS.remove(uploadPath);
          uploadBytes = 0;
          uploadPath = "";
          uploadError = "File too large (max 1 MB)";
          return;
        }
        uploadFile.write(data, len);
        uploadBytes += len;
      }
      if (final && uploadFile) uploadFile.close();
    });

  server.on("/files-json", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    std::vector<String> disk = scanFiles();
    String json = "[";
    json.reserve(disk.size() * 90 + 16);
    bool first = true;
    auto emit = [&](const String &name, bool en, uint16_t dur, size_t sz) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((unsigned)sz) +
              ",\"en\":" + (en ? "true" : "false") + ",\"dur\":" + String(dur) + "}";
    };
    for (auto &e : playlist) {
      if (!plHas(disk, e.name)) continue;
      size_t sz = 0;
      File f = LittleFS.open("/" + e.name, "r");
      if (f) { sz = f.size(); f.close(); }
      emit(e.name, e.en, e.dur, sz);
    }
    for (auto &n : disk) {
      if (plIndex(n) >= 0) continue;
      size_t sz = 0;
      File f = LittleFS.open("/" + n, "r");
      if (f) { sz = f.size(); f.close(); }
      emit(n, true, 0, sz);
    }
    json += "]";
    r->send(200, "application/json", json);
  });

  // Reorder / enable / per-file hold time.
  server.on("/pl", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    String name = r->hasParam("f") ? stripSlash(r->getParam("f")->value()) : String("");
    int i = plIndex(name);
    if (i < 0) { r->send(404, "text/plain", "Not in playlist"); return; }

    if (r->hasParam("up") && i > 0) {
      std::swap(playlist[i], playlist[i - 1]);
    } else if (r->hasParam("down") && i + 1 < (int)playlist.size()) {
      std::swap(playlist[i], playlist[i + 1]);
    } else if (r->hasParam("en")) {
      playlist[i].en = (r->getParam("en")->value() != "0");
    } else if (r->hasParam("dur")) {
      playlist[i].dur = (uint16_t)constrain(r->getParam("dur")->value().toInt(), 0, 3600);
    } else {
      r->send(400, "text/plain", "No action");
      return;
    }
    savePlaylist();
    r->send(200, "text/plain", "OK");
  });

  server.on("/rename", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    if (!r->hasParam("f") || !r->hasParam("to")) { r->send(400, "text/plain", "Missing name"); return; }
    String from = "/" + stripSlash(r->getParam("f")->value());
    String to = stripSlash(r->getParam("to")->value());
    int dot = from.lastIndexOf('.');
    if (to.indexOf('.') < 0 && dot > 0) to += from.substring(dot);
    to = "/" + sanitizeFilename(to);
    if (!LittleFS.exists(from)) { r->send(404, "text/plain", "No such file"); return; }
    if (LittleFS.exists(to)) { r->send(409, "text/plain", "Name already used"); return; }
    PlaybackLock guard;
    bool wasPlaying = (currentFile == from);
    if (wasPlaying) stopPlayback(false, false);
    if (!LittleFS.rename(from, to)) { r->send(500, "text/plain", "Rename failed"); return; }
    int i = plIndex(stripSlash(from));
    if (i >= 0) playlist[i].name = stripSlash(to);
    savePlaylist();
    if (wasPlaying) { prefs.putString("last", to); playWithFade(to); }
    r->send(200, "text/plain", "OK");
  });

  server.on("/delall", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    stopPlayback(true);
    for (auto &n : scanFiles()) LittleFS.remove("/" + n);
    playlist.clear();
    savePlaylist();
    r->send(200, "text/plain", "OK");
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    String j = "{";
    j += "\"fw\":\"" FW_VERSION "\"";
    j += ",\"brightness\":" + String(currentBrightness);
    j += ",\"auto\":" + String(autoRotate ? "true" : "false");
    j += ",\"shuffle\":" + String(shuffleMode ? "true" : "false");
    j += ",\"rotate\":" + String(rotateSeconds);
    j += ",\"pwr\":" + String(powerLimit);
    j += ",\"fade\":" + String(fadeEnabled ? "true" : "false");
    j += ",\"rot\":" + String(panelRotation);
    j += ",\"clockFace\":" + String(clockFace);
    j += ",\"clockDate\":" + String(clockDate ? "true" : "false");
    j += ",\"tz\":\"" + timeZone + "\"";
    j += ",\"sched\":" + String(schedEnabled ? "true" : "false");
    j += ",\"schedOn\":" + String(schedOnMin);
    j += ",\"schedOff\":" + String(schedOffMin);
    j += "}";
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", j);
    res->addHeader("Content-Disposition", "attachment; filename=\"ledmatrix-config.json\"");
    r->send(res);
  });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    prefs.clear();
    r->send(200, "text/plain", "Settings erased - rebooting");
    rebootPending = true;
    rebootAt = millis() + 900;
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    PlaybackLock guard;
    if (!authed(r)) { r->send(401, "text/plain", "PIN required"); return; }
    r->send(200, "application/json", buildStatusJson());
  });

  // Diagnostics: reachable without the PIN so it always helps when locked out.
  server.on("/diag", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", buildDiagJson());
  });

  server.on("/play", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    if (!r->hasParam("f")) { r->send(400); return; }
    String fname = r->getParam("f")->value();
    if (!fname.startsWith("/")) fname = "/" + fname;
    bool ok = playWithFade(fname);
    r->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *r){ PlaybackLock guard; if (!authed(r)) { r->send(401, "text/plain", "PIN required"); return; } stopPlayback(true); r->send(200); });

  server.on("/authstate", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!authed(r)) { r->send(401, "text/plain", "PIN required"); return; }
    String json = "{\"pin\":" + String(adminPin.length() ? "true" : "false") +
                  ",\"token\":\"" + authToken + "\"}";
    r->send(200, "application/json", json);
  });

  server.on("/auth", HTTP_GET, [](AsyncWebServerRequest *r) {
    String p = r->hasParam("pin") ? r->getParam("pin")->value() : String("");
    if (adminPin.length() && p != adminPin) {
      r->send(403, "text/plain", "Wrong PIN");
      return;
    }
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", "OK");
    res->addHeader("Set-Cookie", "auth=" + authToken + "; Path=/; Max-Age=31536000; SameSite=Lax");
    r->send(res);
  });

  server.on("/qr", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!authed(r)) { r->send(401, "text/plain", "PIN required"); return; }
    PlaybackLock guard;
    drawQrScreen(uiUrl());
    r->send(200, "text/plain", uiUrl());
  });

  server.on("/pin", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    if (r->hasParam("off")) {
      adminPin = "";
      prefs.putString("pin", "");
    } else if (r->hasParam("set")) {
      String p = r->getParam("set")->value();
      p.trim();
      if (p.length() < 4 || p.length() > 16) {
        r->send(400, "text/plain", "PIN must be 4-16 characters");
        return;
      }
      adminPin = p;
      prefs.putString("pin", p);
    }
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", "OK");
    res->addHeader("Set-Cookie", "auth=" + authToken + "; Path=/; Max-Age=31536000; SameSite=Lax");
    r->send(res);
  });

  server.on("/ctl", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    String a = r->hasParam("a") ? r->getParam("a")->value() : String("");
    String v = r->hasParam("v") ? r->getParam("v")->value() : String("");
    if (a == "toggle") {
      togglePause();
    } else if (a == "pause") {
      pausePlayback();
    } else if (a == "resume") {
      resumePlayback();
    } else if (a == "next") {
      playRelative(1);
    } else if (a == "prev") {
      playRelative(-1);
    } else if (a == "auto") {
      autoRotate = (v != "0");
      if (autoRotate) {
        if (!paused) {                // give the current file a full interval
          playStartedAt = millis();
          pausedAccumMs = 0;
        }
        if (!currentFile.length()) playRelative(1);
      }
      markPrefsDirty();
    } else if (a == "shuffle") {
      shuffleMode = (v != "0");
      markPrefsDirty();
    } else if (a == "interval") {
      int s = v.toInt();
      if (s < 5) s = 5;
      if (s > 3600) s = 3600;
      rotateSeconds = (uint16_t)s;
      markPrefsDirty();
    } else {
      r->send(400, "text/plain", "Unknown action");
      return;
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/del", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    if (!r->hasParam("f")) { r->send(400); return; }
    String p = "/" + stripSlash(r->getParam("f")->value());
    if (currentFile == p) stopPlayback(true);
    bool ok = LittleFS.remove(p);
    if (ok) syncPlaylist();
    r->send(ok ? 200 : 500);
  });

  server.on("/bright", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      v = constrain(v, 1, 255);
      currentBrightness = v;
      applyBrightness();
      markPrefsDirty();
    }
    r->send(200);
  });

  server.on("/clock", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    if (r->hasParam("off")) {
      stopClockMode(true);
      r->send(200, "text/plain", "OK");
      return;
    }
    if (r->hasParam("face")) {
      clockFace = (uint8_t)constrain(r->getParam("face")->value().toInt(), 0, 1);
      markPrefsDirty();
    }
    if (r->hasParam("date")) {
      clockDate = (r->getParam("date")->value() != "0");
      markPrefsDirty();
    }
    if (r->hasParam("tz")) {
      String tz = r->getParam("tz")->value();
      if (tz.length() && tz.length() <= 48) {
        timeZone = tz;
        configTzTime(timeZone.c_str(), "pool.ntp.org", "time.google.com");
        markPrefsDirty();
      }
    }
    if (r->hasParam("sync")) configTzTime(timeZone.c_str(), "pool.ntp.org", "time.google.com");
    startClockMode();
    r->send(200, "text/plain", "OK");
  });

  server.on("/schedule", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    // Times are "HH:MM" in 24 h local time.
    auto parseHm = [](const String &s, uint16_t &out) -> bool {
      int c = s.indexOf(':');
      if (c < 1) return false;
      int h = s.substring(0, c).toInt();
      int m = s.substring(c + 1).toInt();
      if (h < 0 || h > 23 || m < 0 || m > 59) return false;
      out = (uint16_t)(h * 60 + m);
      return true;
    };
    if (r->hasParam("on")) {
      schedEnabled = (r->getParam("on")->value() != "0");
      markPrefsDirty();
    }
    if (r->hasParam("from")) parseHm(r->getParam("from")->value(), schedOnMin);
    if (r->hasParam("to"))   parseHm(r->getParam("to")->value(), schedOffMin);
    lastSchedCheck = 0;   // re-evaluate on the next loop pass
    markPrefsDirty();
    r->send(200, "text/plain", "OK");
  });

  server.on("/panel", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    PlaybackLock guard;
    if (r->hasParam("pwr")) {
      powerLimit = (uint8_t)constrain(r->getParam("pwr")->value().toInt(), 20, 100);
      applyBrightness();
      markPrefsDirty();
    }
    if (r->hasParam("fade")) {
      fadeEnabled = (r->getParam("fade")->value() != "0");
      markPrefsDirty();
    }
    if (r->hasParam("rot")) {
      uint8_t rot = (uint8_t)constrain(r->getParam("rot")->value().toInt(), 0, 3);
      if (rot != panelRotation) {
        panelRotation = rot;
        if (displayOk) dma_display->setRotation(panelRotation);
        redrawCurrentMode();
        applyBrightness();
        markPrefsDirty();
      }
    }
    if (r->hasParam("qr")) {
      scanQrAtBoot = (r->getParam("qr")->value() != "0");
      prefs.putBool("qr", scanQrAtBoot);
    }
    if (r->hasParam("b")) {
      currentBrightness = (uint8_t)constrain(r->getParam("b")->value().toInt(), 1, 255);
      applyBrightness();
      markPrefsDirty();
    }
    redrawCurrentMode();
    r->send(200, "text/plain", "OK");
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (gate(r)) return;
    if (!r->hasParam("ssid")) { r->send(400, "text/plain", "Missing ssid"); return; }
    String s = r->getParam("ssid")->value();
    String p = r->hasParam("pass") ? r->getParam("pass")->value() : String("");
    if (s.length() == 0 || s.length() > 32 || p.length() > 64) {
      r->send(400, "text/plain", "Invalid credentials");
      return;
    }
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    r->send(200, "text/plain", "Saved - rebooting");
    rebootPending = true;
    rebootAt = millis() + 800;
    Serial.printf("[WIFI] new credentials for '%s', rebooting\n", s.c_str());
  });

  // ---- boot QR screen + live status socket ----
  server.onNotFound([](AsyncWebServerRequest *r) {
    // Captive portal: send any unknown host/path to the UI (works on STA and AP).
    if (apFallback && (uint32_t)WiFi.softAPIP() != 0 && r->method() == HTTP_GET) {
      r->redirect(uiUrl() + "/");
      return;
    }
    r->send(404, "text/plain", "Not found");
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // ---- network, then the listener ---------------------------------------
  // Order is dictated by RAM. AsyncTCP's service task needs a 16 KB
  // contiguous stack and begin() fails *silently* without it (port 80 simply
  // stays closed). So the boot reservation is handed back first, then the
  // radio comes up, then port 80 is opened - all before any association or AP
  // work. The old order ran the AP switch with 99 KB of framebuffers plus the
  // reserve still live, and a failed malloc for the wifi log lock aborted.
  setBootStage(40);

  // Do not let a Wi-Fi driver error log abort the boot: when the heap is tight
  // its ESP_LOG path allocates a stdio lock and abort()s if that fails.
  esp_log_level_set("wifi", ESP_LOG_NONE);

  releaseHeap();
  WiFi.mode(WIFI_STA);              // also initialises lwIP/esp_netif
  WiFi.setHostname("ledmatrix");

  bool listening = false;
  for (int attempt = 1; attempt <= 3 && !listening; attempt++) {
    server.begin();
    delay(120);
    listening = (xTaskGetHandle("async_tcp") != nullptr);
    Serial.printf("[WEB] begin #%d: async_tcp=%s heap=%u largest=%u\n",
                  attempt, listening ? "up" : "MISSING",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!listening) delay(300);
  }
  if (!listening) Serial.println("[WEB] SERVER NOT LISTENING - port 80 is closed");
  serverListening = listening;
  listenRetryAt = millis() + 5000;

  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) { delay(500); Serial.print("."); }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] station failed (status=%d), AP fallback\n", (int)WiFi.status());
    startApFallback();
  } else {
    WiFi.setSleep(false);   // no modem sleep once associated (latency)
  }

  configTzTime(timeZone.c_str(), "pool.ntp.org", "time.google.com");
  esp_log_level_set("wifi", ESP_LOG_ERROR);   // driver logging back to normal
  setBootStage(50);

  if (displayOk) {
    // Keep boot visuals dim so a weak PSU cannot brown the board out.
    uint8_t bootBright = effectiveBrightness();
    if (bootBright > 60) bootBright = 60;
    setPanelBrightness(bootBright);
    dma_display->clearScreen();
    dma_display->setTextColor(dma_display->color565(255, 255, 255));
    dma_display->setCursor(2, 2);
    dma_display->print("LED Matrix");

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
      dma_display->setCursor(2, 16);
      dma_display->setTextColor(dma_display->color565(0, 200, 0));
      dma_display->print("WiFi OK");
      dma_display->setCursor(2, 28);
      dma_display->setTextColor(dma_display->color565(255, 255, 255));
      dma_display->print(WiFi.localIP().toString());
    } else {
      bool apUp = ((uint32_t)WiFi.softAPIP() != 0);
      Serial.printf("[WIFI] FAILED - %s at %s\n",
                    apUp ? apSsid.c_str() : "AP off",
                    apUp ? WiFi.softAPIP().toString().c_str() : "-");
      dma_display->setCursor(2, 16);
      dma_display->setTextColor(dma_display->color565(255, 180, 0));
      dma_display->print(apUp ? "AP mode" : "no WiFi");
      dma_display->setCursor(2, 28);
      dma_display->setTextColor(dma_display->color565(255, 255, 255));
      dma_display->print(apUp ? apSsid : "192.168.0.113");
      dma_display->setCursor(2, 40);
      if (apUp) dma_display->print("192.168.4.1");
    }
    presentFrame();
  }

  if (listening && MDNS.begin("ledmatrix")) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.println("[MDNS] http://ledmatrix.local/");
  }

  setBootStage(80);
  Serial.printf("[WEB] ready on http://%s/  (AP: %s)\n",
                WiFi.localIP().toString().c_str(),
                apFallback ? WiFi.softAPIP().toString().c_str() : "-");

  // ---- boot QR screen so the UI URL is easy to reach ----
  if (scanQrAtBoot && !safeMode && displayOk) {
    Serial.printf("[QR] %s\n", uiUrl().c_str());
    setBootStage(85);
    drawQrScreen(uiUrl());
    delay(3200);
    showingQr = false;
  }
  if (displayOk) applyBrightness();   // back to the user's brightness after boot visuals

  // ---- resume last file (unless the user stopped it explicitly) ----
  String last = prefs.getString("last", "");
  if (safeMode) {
    Serial.printf("[BOOT] safe mode: not resuming '%s'\n", last.c_str());
  } else if (last.length()) {
    setBootStage(90);
    if (LittleFS.exists(last)) {
      if (!playFile(last)) Serial.printf("[BOOT] resume failed '%s'\n", last.c_str());
    } else {
      Serial.printf("[BOOT] last file '%s' missing, clearing\n", last.c_str());
      prefs.remove("last");
    }
  }
  setBootStage(100);
  disarmBootGuard();
  minFreeHeap = ESP.getFreeHeap();
  fpsWindowStart = millis();
}

// ==================== LOOP ====================
void loop() {
  // Once we have stayed up for a while, the last boot is no longer a "fast restart".
  if (bootCount && (millis() - bootStartedAt) > 20000UL) { bootCount = 0; bootMarker = 0; }
  uint32_t freeNow = ESP.getFreeHeap();
  if (freeNow < minFreeHeap) minFreeHeap = freeNow;

  servicePrefs();
  serviceUpload();
  serviceWifi();

  if (apFallback) dnsServer.processNextRequest();
  ws.cleanupClients();

  // If the HTTP listener could not be created at boot (low contiguous heap),
  // keep trying: AsyncTCP needs a 16 KB stack and frees up as things settle.
  if (!serverListening && (int32_t)(millis() - listenRetryAt) >= 0) {
    listenRetryAt = millis() + 5000;
    server.begin();
    serverListening = (xTaskGetHandle("async_tcp") != nullptr);
    Serial.printf("[WEB] listen retry: %s heap=%u largest=%u\n",
                  serverListening ? "up" : "still closed",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (serverListening && !mdnsStarted && MDNS.begin("ledmatrix")) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
  }

  // Schedule: on/off window, checked twice a second at most.
  uint32_t now = millis();
  if (now - lastSchedCheck >= 500) {
    lastSchedCheck = now;
    checkSchedule();
  }

  // Live status over WebSocket (authenticated clients only).
  if (now - lastWsPush >= 1000) {
    lastWsPush = now;
    broadcastStatus();
  }
  serviceHttpWatchdog();

  // Deferred reboot (after a Wi-Fi or settings change).
  if (rebootPending && (int32_t)(millis() - rebootAt) >= 0) {
    Serial.println("[SYS] rebooting");
    restartNow();
  }

  // While the schedule has the panel off, only the housekeeping above keeps running.
  if (displayAsleep) {
    delay(20);
    return;
  }

  // The clock owns the panel while shown; otherwise auto-rotate the playlist.
  if (showingQr) {
    if (qrUntil && (int32_t)(now - qrUntil) >= 0) {
      showingQr = false;
      qrUntil = 0;
      redrawCurrentMode();
    }
  } else if (showingClock) {
    serviceClock();
  } else if (autoRotate && !paused && currentFile.length()) {
    PlaybackLock guard;
    if (playbackElapsedMs() >= currentHoldMs()) {
      std::vector<String> files = listMediaFiles();
      if (files.size() > 1) playRelative(1);
      else playStartedAt = millis();   // single file: just reset the timer
    }
  }

  if (playingGif && !paused && !showingClock && !showingQr &&
      millis() >= nextFrameTime) {
    PlaybackLock guard;
    int frameDelay = 0;
    if (!gif.playFrame(false, &frameDelay)) {
      gif.close();
      playingGif = false;
      if (!restartGif(currentFile)) {
        Serial.println("[LOOP] restart failed - stopping");
        currentFile = "";
        clearPanel();
      }
    } else {
      frameCount++;
      fpsFrames++;
      pushGifCanvas();
      nextFrameTime = millis() + (frameDelay > 0 ? frameDelay : 20);
    }
  }

  // FPS averaged over ~1 s windows.
  if (now - fpsWindowStart >= 1000) {
    uint32_t delta = now - fpsWindowStart;
    currentFps = (uint16_t)(fpsFrames * 1000UL / delta);
    fpsFrames = 0;
    fpsWindowStart = now;
  }

  delay(1);
}
