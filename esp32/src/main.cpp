#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

// Local credentials: copy include/secrets.example.h to include/secrets.h (git-ignored)
// and put your WiFi and OTA password there. Without that file, empty defaults are used
// and the ESP boots into the emergency AP to be configured from its web page.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID ""
#endif
#ifndef SECRET_WIFI_PASSWORD
#define SECRET_WIFI_PASSWORD ""
#endif
#ifndef SECRET_OTA_PASSWORD
#define SECRET_OTA_PASSWORD ""
#endif

const uint32_t SERIAL_BAUD_RATE = 115200;
const uint16_t HTTP_PORT = 80;
const uint16_t OTA_PORT = 3232;
const uint16_t OLED_WIDTH = 128;
const uint16_t OLED_HEIGHT = 64;
const uint32_t WIFI_RETRY_DELAY_MS = 500;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
// Margin over the HTTPClient 5s default: the service can take several seconds to
// build a new frame (cache miss) and a short timeout triggers
// HTTPC_ERROR_READ_TIMEOUT (-11) and leaves the screen in ellipsis mode.
const uint16_t HTTP_CLIENT_TIMEOUT_MS = 15000;
// Separate short connect timeout: if the service host is powered off (does not
// reject, it simply never answers the SYN), connect() blocks until the timeout
// expires. With the screensaver active that freezes the animation, so the probe
// must fail fast and keep the long read timeout only for slow frames.
const uint16_t HTTP_CONNECT_TIMEOUT_MS = 2000;
// Activity long-poll: non-blocking connection the ESP keeps open to react
// instantly (thinking/done/needs attention) without polling the frame.
const uint16_t ACTIVITY_CONNECT_TIMEOUT_MS = 3000;
const uint32_t ACTIVITY_LONGPOLL_TIMEOUT_MS = 30000;
const uint32_t ACTIVITY_LONGPOLL_RETRY_MS = 1000;
const size_t ACTIVITY_RESPONSE_MAX_BYTES = 512;

// Screensaver (anti burn-in when the service is down). Ids aligned with the
// service (config SCREENSAVERS) and the X-Saver header.
const uint8_t SAVER_BLACK = 0;
const uint8_t SAVER_SNAKE = 1;
const uint8_t SAVER_PIPES = 2;
const uint8_t SAVER_MATRIX = 3;
const uint8_t SAVER_DVD = 4;
const uint8_t SAVER_MAZE = 5;
const uint8_t SAVER_FLOWER = 6;
const uint8_t SAVER_DEFAULT = SAVER_DVD;
// After this time without service, the ESP switches to the screensaver instead of a fixed image.
const uint32_t SAVER_GRACE_MS = 15000;
const uint32_t SAVER_FRAME_MS = 40;
const uint32_t RESTART_DELAY_MS = 1200;
const uint32_t DEFAULT_POLL_INTERVAL_MS = 10000;
const uint32_t PERIODIC_STATUS_LOG_INTERVAL_MS = 60000;
const char ESP_HOSTNAME[] = "esp32";
const char LOCAL_DOMAIN[] = "esp32.local";
const char OTA_HOSTNAME[] = "esp32";
const char OTA_PASSWORD[] = SECRET_OTA_PASSWORD;
const char DEFAULT_WIFI_SSID[] = SECRET_WIFI_SSID;
const char DEFAULT_WIFI_PASSWORD[] = SECRET_WIFI_PASSWORD;
const char EMERGENCY_AP_SSID[] = "ESP32-Emergencia";
const char EMERGENCY_AP_PASSWORD[] = "configesp32";
const char EMERGENCY_AP_URL[] = "http://192.168.4.1";
const char DEFAULT_SERVICE_URL[] = "http://192.168.1.75:8765";
const size_t SERIAL_LOG_MAX_LENGTH = 8000;
const uint8_t BOOT_LOG_MAX_LINES = 8;
const uint8_t BOOT_LOG_MAX_CHARS = 21;
// Boot screen: mascot on the left + logs on the right inside a frame.
const int16_t BOOT_MASCOT_CX = 22;       // X center of the mascot column
const int16_t BOOT_MASCOT_CY = 24;       // Y center of the mascot
const int16_t BOOT_DIVIDER_X = 43;       // vertical divider between mascot and logs
const int16_t BOOT_LOG_X = 47;           // horizontal start of the logs
const int16_t BOOT_LOG_TOP = 4;          // top margin of the logs
const uint8_t BOOT_LOG_LINE_CHARS = 13;  // characters per line next to the mascot
const uint8_t BOOT_LOG_LINE_COUNT = 7;   // visible log lines next to the mascot

// Box reserved in the top-right corner for the activity animation.
// The real size (width x height) arrives in the header of the packet the service sends
// (frame_width/frame_height from config) so it can be adjusted dynamically. The box
// is anchored to the right edge: X = OLED_WIDTH - frameW, Y = 0.
const uint16_t ACTIVITY_BOX_Y = 0;
// Cap the firmware accepts from the packet (must match ANIM_FRAME_MAX_* on the
// service). If the service asks for more, the packet is dropped as out of range.
const uint16_t ACTIVITY_BOX_MAX_W = OLED_WIDTH;
const uint16_t ACTIVITY_BOX_MAX_H = 16;

// Binary animation packet the ESP downloads and stores (not reflashed).
const uint8_t ANIM_PACK_MAGIC = 0xA1;
const uint8_t ANIM_PACK_VERSION = 1;
const uint8_t ANIM_PACK_HEADER_BYTES = 10;
// Buffer sized for the maximum configurable box (128x16) with the maximum number of
// frames (32, worm style): ceil(128/8)*16*32 = 8192 bytes.
const size_t ANIM_MAX_BYTES = 8192;
const uint8_t ANIM_MAX_FRAMES = 32;

struct DisplaySettings
{
    bool enabled;
    uint8_t sdaPin;
    uint8_t sclPin;
    uint8_t address;
    uint8_t contrast;
};

struct WifiSettings
{
    String ssid;
    String password;
};

struct EspSettings
{
    WifiSettings wifi;
    String serviceUrl;
    uint32_t pollIntervalMs;
    DisplaySettings claudeDisplay;
    DisplaySettings codexDisplay;
};

struct MonitorState
{
    bool serviceOnline;
    String serviceError;
    uint32_t lastPollMs;
    uint32_t lastSuccessMs;
    uint64_t frameUpdatedAtMs;
};

WebServer server(HTTP_PORT);
Preferences preferences;
TwoWire claudeWire = TwoWire(0);
TwoWire codexWire = TwoWire(1);
Adafruit_SSD1306 claudeOled(OLED_WIDTH, OLED_HEIGHT, &claudeWire, -1);
Adafruit_SSD1306 codexOled(OLED_WIDTH, OLED_HEIGHT, &codexWire, -1);

EspSettings settings;
MonitorState monitorState;
bool claudeDisplayReady = false;
bool codexDisplayReady = false;
bool webOtaError = false;
bool webOtaRestartPending = false;
uint32_t webOtaRestartAtMs = 0;
String webOtaStatus = "Ready to upload firmware.";
size_t webOtaBytes = 0;
String serialLogBuffer = "";

// Framebuffers the PC draws and the ESP32 only projects (128x64 = 1024 bytes).
const size_t FRAME_BYTES = 1024;
const size_t FRAMES_TOTAL_BYTES = FRAME_BYTES * 2;
uint8_t claudeFrame[FRAME_BYTES];
uint8_t codexFrame[FRAME_BYTES];
bool framesReceived = false;
String frameEtag = "";
uint8_t ellipsisStep = 0;

// Activity animation: frames + parameters the service defines and the ESP stores.
struct ActivityAnimation
{
    bool valid;
    uint8_t frameW;
    uint8_t frameH;
    uint8_t frameCount;
    uint16_t frameBytes;
    uint16_t intervalMs;
    uint16_t blinkMs;
    bool invertOnWaiting;
};

// Runtime state of the animation/inversion per physical screen.
struct DisplayActivityRuntime
{
    bool animActive;
    uint8_t animIndex;
    uint32_t lastAnimStepMs;
    bool invertOn;
    uint32_t lastInvertToggleMs;
};

uint8_t animFrameData[ANIM_MAX_BYTES];
ActivityAnimation activityAnimation = {false, 0, 0, 0, 0, 120, 600, true};
String activityAnimationEtag = "";
// Latest animation version announced by the service; if it differs, it is re-downloaded.
String pendingAnimEtag = "";
// Per-content activity state as reported by the service (idle/busy/waiting).
String claudeContentActivity = "idle";
String codexContentActivity = "idle";
// Sessions working (busy) in parallel per tool, per the X-Sessions-* headers.
// They determine how many dots the ESP draws on the right edge: 2 dots per session.
uint8_t claudeBusySessions = 1;
uint8_t codexBusySessions = 1;
// Activity animation style configured on the service (X-Anim-Style header). The
// discrete styles (dots, dots-right, stars-right, bars) encode the session count;
// the rest (spinner, pulse, ball, wave, worm) play as a pure animation from the packet.
String activityStyle = "dots-right";
DisplayActivityRuntime claudeRuntime = {false, 0, 0, false, 0};
DisplayActivityRuntime codexRuntime = {false, 0, 0, false, 0};
bool liveLogDisplayEnabled = true;
bool emergencyWifiActive = false;
bool configRestartPending = false;
uint32_t configRestartAtMs = 0;
uint32_t lastNoChangeLogAtMs = 0;

// State of the activity long-poll client (non-blocking).
WiFiClient activityClient;
bool activityPollActive = false;
uint32_t activityPollStartMs = 0;
uint32_t activityPollNextStartMs = 0;
String activityResponse = "";

// State of the frames client (non-blocking): same pattern as the activity long-poll.
// Previously the frame fetch used a synchronous HTTPClient.GET() that blocked the loop -and
// therefore the display animation- while the server processed the response and the 2048
// bytes were read; that caused the ~1s freeze every pollInterval. Now it connects, sends
// the GET and reads in chunks on each loop pass, without stalling the render.
const uint16_t FRAMES_CONNECT_TIMEOUT_MS = 2000;
const uint32_t FRAMES_FETCH_TIMEOUT_MS = 8000;
const size_t FRAMES_RESP_MAX_BYTES = FRAMES_TOTAL_BYTES + 1024;
WiFiClient framesClient;
bool framesFetchActive = false;
uint32_t framesFetchStartMs = 0;
uint8_t framesRespBuf[FRAMES_RESP_MAX_BYTES];
size_t framesRespLen = 0;

// Screensaver state per physical screen (anti burn-in when the service is down).
struct SaverState
{
    bool initialized;
    uint32_t frame;
    uint32_t rng;
    int16_t dvdX, dvdY, dvdVX, dvdVY; // DVD (corner) and center of the flower
    uint8_t snakeX[48], snakeY[48], snakeLen, foodX, foodY;
    int16_t drop[22];
    uint8_t dropSpeed[22];
    uint8_t pipeX, pipeY, pipeDir;
    uint8_t mazeWallE[16], mazeWallS[16], mazeX, mazeY;
};

SaverState claudeSaver = {};
SaverState codexSaver = {};
uint8_t currentSaverId = SAVER_DEFAULT;
bool saverRunning = false;
uint32_t lastSaverFrameMs = 0;
// Current brightness (% of the configured contrast); the service decides it via X-Brightness.
uint8_t currentBrightnessPct = 100;

void drawBootLogContent(Adafruit_SSD1306 &display, const bool isClaude);
void drawBootLogs(Adafruit_SSD1306 &display, const bool isClaude);
void drawBootLogDisplays();
void drawMascotClaude(Adafruit_SSD1306 &display, const int16_t cx, const int16_t cy);
void drawMascotCodex(Adafruit_SSD1306 &display, const int16_t cx, const int16_t cy);
void drawEmergencyWifiDisplays();
void renderFrames();
void drawWaitingOverlay(Adafruit_SSD1306 &display);
void drawOfflineDisplays(const String &reason);
void serviceFramesFetch();
bool parseServiceHostPort(const String &url, String &host, uint16_t &port);
void fetchActivityAnimation();
void updateActivityEffects();
void stopActivityLongPoll(uint32_t retryAtMs);
void serviceActivityLongPoll();
uint8_t parseSaverId(const String &name);
void startScreenSaver();
void renderScreenSaver();
void applyBrightness(uint8_t pct);
void logPrint(const String &message);
void logPrintln(const String &message);
void logPrintlnEmpty();

String boolToJson(const bool value)
{
    if (value)
    {
        return "true";
    }

    return "false";
}

String uint64ToString(const uint64_t value)
{
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));

    return String(buffer);
}

String stringToJson(const String &value)
{
    String escapedValue = value;
    escapedValue.replace("\\", "\\\\");
    escapedValue.replace("\"", "\\\"");
    escapedValue.replace("\n", "\\n");
    escapedValue.replace("\r", "\\r");
    escapedValue.replace("\t", "\\t");

    return String("\"") + escapedValue + String("\"");
}

void trimSerialLogBuffer()
{
    const size_t currentLength = serialLogBuffer.length();
    if (currentLength <= SERIAL_LOG_MAX_LENGTH)
    {
        return;
    }

    serialLogBuffer.remove(0, currentLength - SERIAL_LOG_MAX_LENGTH);
}

void appendSerialLog(const String &message)
{
    serialLogBuffer += message;
    trimSerialLogBuffer();
    drawBootLogDisplays();
}

void logPrint(const String &message)
{
    Serial.print(message);
    appendSerialLog(message);
}

void logPrintln(const String &message)
{
    Serial.println(message);
    appendSerialLog(message + String("\n"));
}

void logPrintlnEmpty()
{
    Serial.println();
    appendSerialLog(String("\n"));
}

DisplaySettings buildDisplaySettings(const bool enabled, const uint8_t sdaPin, const uint8_t sclPin)
{
    DisplaySettings displaySettings;
    displaySettings.enabled = enabled;
    displaySettings.sdaPin = sdaPin;
    displaySettings.sclPin = sclPin;
    displaySettings.address = 0x3C;
    displaySettings.contrast = 180;

    return displaySettings;
}

void loadSettings()
{
    preferences.begin("monitor", false);
    settings.wifi.ssid = preferences.isKey("wifi_ssid") ? preferences.getString("wifi_ssid") : DEFAULT_WIFI_SSID;
    settings.wifi.password = preferences.isKey("wifi_pass") ? preferences.getString("wifi_pass") : DEFAULT_WIFI_PASSWORD;
    settings.serviceUrl = preferences.isKey("service_url") ? preferences.getString("service_url") : DEFAULT_SERVICE_URL;
    settings.pollIntervalMs = preferences.isKey("poll_ms") ? preferences.getUInt("poll_ms") : DEFAULT_POLL_INTERVAL_MS;
    settings.claudeDisplay.enabled = preferences.isKey("claude_en") ? preferences.getBool("claude_en") : true;
    settings.claudeDisplay.sdaPin = preferences.isKey("claude_sda") ? preferences.getUChar("claude_sda") : 21;
    settings.claudeDisplay.sclPin = preferences.isKey("claude_scl") ? preferences.getUChar("claude_scl") : 22;
    settings.claudeDisplay.address = preferences.isKey("claude_addr") ? preferences.getUChar("claude_addr") : 0x3C;
    settings.claudeDisplay.contrast = preferences.isKey("claude_ctr") ? preferences.getUChar("claude_ctr") : 180;
    settings.codexDisplay.enabled = preferences.isKey("codex_en") ? preferences.getBool("codex_en") : true;
    settings.codexDisplay.sdaPin = preferences.isKey("codex_sda") ? preferences.getUChar("codex_sda") : 16;
    settings.codexDisplay.sclPin = preferences.isKey("codex_scl") ? preferences.getUChar("codex_scl") : 17;
    settings.codexDisplay.address = preferences.isKey("codex_addr") ? preferences.getUChar("codex_addr") : 0x3C;
    settings.codexDisplay.contrast = preferences.isKey("codex_ctr") ? preferences.getUChar("codex_ctr") : 180;
    currentSaverId = preferences.isKey("saver") ? preferences.getUChar("saver") : SAVER_DEFAULT;
    preferences.end();
}

void saveSettings()
{
    preferences.begin("monitor", false);
    preferences.putString("wifi_ssid", settings.wifi.ssid);
    preferences.putString("wifi_pass", settings.wifi.password);
    preferences.putString("service_url", settings.serviceUrl);
    preferences.putUInt("poll_ms", settings.pollIntervalMs);
    preferences.putBool("claude_en", settings.claudeDisplay.enabled);
    preferences.putUChar("claude_sda", settings.claudeDisplay.sdaPin);
    preferences.putUChar("claude_scl", settings.claudeDisplay.sclPin);
    preferences.putUChar("claude_addr", settings.claudeDisplay.address);
    preferences.putUChar("claude_ctr", settings.claudeDisplay.contrast);
    preferences.putBool("codex_en", settings.codexDisplay.enabled);
    preferences.putUChar("codex_sda", settings.codexDisplay.sdaPin);
    preferences.putUChar("codex_scl", settings.codexDisplay.sclPin);
    preferences.putUChar("codex_addr", settings.codexDisplay.address);
    preferences.putUChar("codex_ctr", settings.codexDisplay.contrast);
    preferences.end();
}

String getSettingsJson()
{
    return String("{") +
           String("\"wifi\":{\"ssid\":") + stringToJson(settings.wifi.ssid) +
           String(",\"has_password\":") + boolToJson(settings.wifi.password.length() > 0) +
           String("},\"service_url\":") + stringToJson(settings.serviceUrl) +
           String(",\"poll_interval_ms\":") + String(settings.pollIntervalMs) +
           String(",\"claude_display\":{\"enabled\":") + boolToJson(settings.claudeDisplay.enabled) +
           String(",\"sda\":") + String(settings.claudeDisplay.sdaPin) +
           String(",\"scl\":") + String(settings.claudeDisplay.sclPin) +
           String(",\"address\":") + String(settings.claudeDisplay.address) +
           String(",\"contrast\":") + String(settings.claudeDisplay.contrast) +
           String("},\"codex_display\":{\"enabled\":") + boolToJson(settings.codexDisplay.enabled) +
           String(",\"sda\":") + String(settings.codexDisplay.sdaPin) +
           String(",\"scl\":") + String(settings.codexDisplay.sclPin) +
           String(",\"address\":") + String(settings.codexDisplay.address) +
           String(",\"contrast\":") + String(settings.codexDisplay.contrast) +
           String("}}");
}

String getActiveWifiSsid()
{
    if (emergencyWifiActive)
    {
        return String(EMERGENCY_AP_SSID);
    }

    return WiFi.SSID();
}

String getActiveWifiIp()
{
    if (emergencyWifiActive)
    {
        return WiFi.softAPIP().toString();
    }

    return WiFi.localIP().toString();
}

String getWifiModeText()
{
    if (emergencyWifiActive)
    {
        return "emergency_ap";
    }

    return "station";
}

String getStatusJson()
{
    return String("{\"status\":\"ok\"") +
           String(",\"wifi_mode\":") + stringToJson(getWifiModeText()) +
           String(",\"wifi_ssid\":") + stringToJson(getActiveWifiSsid()) +
           String(",\"ip\":") + stringToJson(getActiveWifiIp()) +
           String(",\"local_domain\":") + stringToJson(LOCAL_DOMAIN) +
           String(",\"emergency_ap_ssid\":") + stringToJson(emergencyWifiActive ? String(EMERGENCY_AP_SSID) : String("")) +
           String(",\"emergency_ap_password\":") + stringToJson(emergencyWifiActive ? String(EMERGENCY_AP_PASSWORD) : String("")) +
           String(",\"emergency_ap_url\":") + stringToJson(emergencyWifiActive ? String(EMERGENCY_AP_URL) : String("")) +
           String(",\"rssi\":") + String(emergencyWifiActive ? 0 : WiFi.RSSI()) +
           String(",\"service_online\":") + boolToJson(monitorState.serviceOnline) +
           String(",\"service_error\":") + stringToJson(monitorState.serviceError) +
           String(",\"service_url\":") + stringToJson(settings.serviceUrl) +
           String(",\"last_poll_ms\":") + String(monitorState.lastPollMs) +
           String(",\"last_success_ms\":") + String(monitorState.lastSuccessMs) +
           String(",\"frame_updated_at_ms\":") + uint64ToString(monitorState.frameUpdatedAtMs) +
           String(",\"claude_display_ready\":") + boolToJson(claudeDisplayReady) +
           String(",\"codex_display_ready\":") + boolToJson(codexDisplayReady) +
           String(",\"web_ota_status\":") + stringToJson(webOtaStatus) +
           String(",\"web_ota_error\":") + boolToJson(webOtaError) +
           String(",\"web_ota_bytes\":") + String(webOtaBytes) +
           String(",\"settings\":") + getSettingsJson() +
           String("}");
}

String getLogsJson()
{
    return String("{\"logs\":") + stringToJson(serialLogBuffer) +
           String(",\"bytes\":") + String(serialLogBuffer.length()) +
           String(",\"max_bytes\":") + String(SERIAL_LOG_MAX_LENGTH) +
           String("}");
}

String getPageHtml()
{
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Monitor</title>
  <style>
    :root { --bg:#fafafa; --panel:#fff; --text:#111; --muted:#666; --line:#e5e5e5; --soft:#f5f5f5; --accent:#111; --ok:#137a3f; --warn:#9a6700; --danger:#b42318; }
    * { box-sizing:border-box; }
    body { margin:0; background:var(--bg); color:var(--text); font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
    .app { min-height:100vh; display:grid; grid-template-columns:220px 1fr; }
    aside { background:var(--panel); border-right:1px solid var(--line); padding:18px 14px; }
    .brand { font-weight:750; margin-bottom:18px; }
    nav { display:grid; gap:4px; }
    nav button { border:0; background:transparent; color:var(--muted); text-align:left; border-radius:6px; padding:9px 10px; font:inherit; cursor:pointer; }
    nav button.active { background:var(--soft); color:var(--text); font-weight:700; }
    main { width:100%; max-width:1120px; padding:24px; }
    header { display:flex; align-items:flex-start; justify-content:space-between; gap:16px; margin-bottom:20px; }
    h1 { margin:0; font-size:26px; line-height:1.15; }
    h2 { margin:0 0 14px; font-size:17px; }
    p { margin:6px 0; color:var(--muted); line-height:1.45; }
    .grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:14px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:16px; }
    .metric { display:flex; align-items:baseline; justify-content:space-between; gap:12px; }
    .pill { display:inline-flex; align-items:center; min-height:24px; padding:0 9px; border:1px solid var(--line); border-radius:999px; color:var(--muted); font-size:12px; font-weight:700; }
    .pill.ok { color:var(--ok); border-color:#b7e4c7; background:#f0fff4; }
    .pill.warn { color:var(--warn); border-color:#f8df9b; background:#fffbeb; }
    .pill.danger { color:var(--danger); border-color:#ffd1cc; background:#fff5f5; }
    .bar { height:8px; border-radius:999px; background:var(--soft); overflow:hidden; margin:14px 0 8px; }
    .bar span { display:block; height:100%; width:0%; background:var(--accent); transition:width .18s ease; }
    form { display:grid; gap:14px; }
    label { display:grid; gap:6px; color:var(--muted); font-size:13px; font-weight:650; }
    input, select { min-height:38px; width:100%; border:1px solid var(--line); border-radius:6px; background:var(--panel); color:var(--text); padding:8px 10px; font:inherit; }
    button.primary { min-height:40px; border:0; border-radius:6px; background:var(--accent); color:#fff; font-weight:750; cursor:pointer; padding:0 14px; }
    .form-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:14px; }
    .page { display:none; }
    .page.active { display:block; }
    pre { margin:0; overflow:auto; background:#0a0a0a; color:#ededed; padding:14px; border-radius:8px; font-size:12px; }
    .log-window { min-height:440px; max-height:62vh; white-space:pre-wrap; word-break:break-word; }
    @media (max-width:760px) { .app{grid-template-columns:1fr;} aside{border-right:0;border-bottom:1px solid var(--line);} nav{grid-template-columns:repeat(4,1fr);} main{padding:16px;} .grid,.form-grid{grid-template-columns:1fr;} header{display:block;} }
  </style>
</head>
<body>
  <div class="app">
    <aside>
      <div class="brand">ESP32 Monitor</div>
      <nav>
        <button class="active" data-page="home">Home</button>
        <button data-page="ota">OTA</button>
        <button data-page="config">Configuration</button>
        <button data-page="logs">Logs</button>
      </nav>
    </aside>
    <main>
      <header>
        <div><h1>Monitor status</h1><p>Usage comes from the local service on your PC.</p></div>
        <span id="service-pill" class="pill">Loading</span>
      </header>
      <section id="page-home" class="page active">
        <div id="status-cards" class="grid"></div>
      </section>
      <section id="page-ota" class="page">
        <div class="panel">
          <h2>Web OTA</h2>
          <p>Upload a .bin firmware. For normal work use USB for speed.</p>
          <form id="ota-form">
            <input id="firmware" name="firmware" type="file" accept=".bin,application/octet-stream" required>
            <div class="bar"><span id="ota-bar"></span></div>
            <p id="ota-status">Ready to upload firmware.</p>
            <button class="primary" type="submit">Upload firmware</button>
          </form>
        </div>
      </section>
      <section id="page-config" class="page">
        <div class="panel">
          <h2>Local ESP configuration</h2>
          <form id="config-form">
            <div class="form-grid">
              <label>WiFi SSID
                <input id="wifi-ssid" type="text" minlength="1" maxlength="32" required>
              </label>
              <label>WiFi password
                <input id="wifi-password" type="password" minlength="8" maxlength="63" placeholder="Leave empty to keep current">
              </label>
              <label>Service URL
                <input id="service-url" type="url" required>
              </label>
              <label>Polling ms
                <input id="poll-ms" type="number" min="1000" step="1000" required>
              </label>
              <label>Claude OLED enabled
                <select id="claude-enabled"><option value="true">Yes</option><option value="false">No</option></select>
              </label>
              <label>Codex OLED enabled
                <select id="codex-enabled"><option value="true">Yes</option><option value="false">No</option></select>
              </label>
              <label>Claude SDA
                <input id="claude-sda" type="number" min="0" max="39">
              </label>
              <label>Claude SCL
                <input id="claude-scl" type="number" min="0" max="39">
              </label>
              <label>Codex SDA
                <input id="codex-sda" type="number" min="0" max="39">
              </label>
              <label>Codex SCL
                <input id="codex-scl" type="number" min="0" max="39">
              </label>
              <label>Claude address decimal
                <input id="claude-address" type="number" min="1" max="127">
              </label>
              <label>Codex address decimal
                <input id="codex-address" type="number" min="1" max="127">
              </label>
              <label>Claude contrast
                <input id="claude-contrast" type="number" min="0" max="255">
              </label>
              <label>Codex contrast
                <input id="codex-contrast" type="number" min="0" max="255">
              </label>
            </div>
            <button class="primary" type="submit">Save and restart displays</button>
          </form>
        </div>
        <div class="panel" style="margin-top:14px"><h2>JSON status</h2><pre id="raw-json">{}</pre></div>
      </section>
      <section id="page-logs" class="page">
        <div class="panel">
          <div class="metric"><h2>Serial logs</h2><span id="serial-log-meta" class="pill">0 bytes</span></div>
          <pre id="serial-logs" class="log-window"></pre>
        </div>
      </section>
    </main>
  </div>
  <script>
    function $(id) { return document.getElementById(id); }
    function escapeHtml(value) {
      return String(value ?? '').replace(/[&<>"']/g, (character) => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[character]));
    }
    function boolLabel(value) {
      return value ? 'Yes' : 'No';
    }
    function renderStatusCards(data) {
      const serviceClass = data.service_online ? 'ok' : 'danger';
      const displayClass = data.claude_display_ready && data.codex_display_ready ? 'ok' : 'warn';
      const emergencyMode = data.wifi_mode === 'emergency_ap';
      const wifiClass = emergencyMode ? 'warn' : 'ok';
      const wifiLabel = emergencyMode ? 'Emergency' : 'Connected';
      const domainLine = emergencyMode ? `<p>Open: ${escapeHtml(data.emergency_ap_url)}</p>` : `<p>Domain: ${escapeHtml(data.local_domain)}</p>`;
      const passwordLine = emergencyMode ? `<p>Password: ${escapeHtml(data.emergency_ap_password)}</p>` : '';
      return `
        <article class="panel">
          <div class="metric"><h2>WiFi</h2><span class="pill ${wifiClass}">${wifiLabel}</span></div>
          <p>Network: ${escapeHtml(data.wifi_ssid)}</p>
          ${passwordLine}
          <p>IP: ${escapeHtml(data.ip)}</p>
          ${domainLine}
          <p>RSSI: ${escapeHtml(data.rssi)} dBm</p>
        </article>
        <article class="panel">
          <div class="metric"><h2>Frames</h2><span class="pill ${serviceClass}">${data.service_online ? 'Online' : 'Offline'}</span></div>
          <p>Service: ${escapeHtml(data.service_url)}</p>
          <p>Error: ${escapeHtml(data.service_error || 'No errors')}</p>
          <p>Last frame: ${escapeHtml(data.frame_updated_at_ms)} ms</p>
        </article>
        <article class="panel">
          <div class="metric"><h2>Displays</h2><span class="pill ${displayClass}">OLED</span></div>
          <p>Claude: ${boolLabel(data.claude_display_ready)}</p>
          <p>Codex: ${boolLabel(data.codex_display_ready)}</p>
        </article>
        <article class="panel">
          <div class="metric"><h2>OTA</h2><span class="pill ${data.web_ota_error ? 'danger' : 'ok'}">${data.web_ota_error ? 'Error' : 'Ready'}</span></div>
          <p>${escapeHtml(data.web_ota_status)}</p>
          <p>Bytes received: ${escapeHtml(data.web_ota_bytes)}</p>
        </article>`;
    }
    async function loadStatus() {
      const response = await fetch('/api/status', { cache: 'no-store' });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    async function loadLogs() {
      const response = await fetch('/api/logs', { cache: 'no-store' });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    async function postConfig(config) {
      const response = await fetch('/api/config', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(config) });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    function fillConfig(data) {
      const s = data.settings;
      $('wifi-ssid').value = s.wifi.ssid;
      $('wifi-password').value = '';
      $('service-url').value = s.service_url;
      $('poll-ms').value = s.poll_interval_ms;
      $('claude-enabled').value = String(s.claude_display.enabled);
      $('codex-enabled').value = String(s.codex_display.enabled);
      $('claude-sda').value = s.claude_display.sda;
      $('claude-scl').value = s.claude_display.scl;
      $('codex-sda').value = s.codex_display.sda;
      $('codex-scl').value = s.codex_display.scl;
      $('claude-address').value = s.claude_display.address;
      $('codex-address').value = s.codex_display.address;
      $('claude-contrast').value = s.claude_display.contrast;
      $('codex-contrast').value = s.codex_display.contrast;
    }
    function readConfig() {
      return {
        wifi: { ssid: $('wifi-ssid').value.trim(), password: $('wifi-password').value },
        service_url: $('service-url').value,
        poll_interval_ms: Number($('poll-ms').value),
        claude_display: { enabled: $('claude-enabled').value === 'true', sda: Number($('claude-sda').value), scl: Number($('claude-scl').value), address: Number($('claude-address').value), contrast: Number($('claude-contrast').value) },
        codex_display: { enabled: $('codex-enabled').value === 'true', sda: Number($('codex-sda').value), scl: Number($('codex-scl').value), address: Number($('codex-address').value), contrast: Number($('codex-contrast').value) }
      };
    }
    async function refresh() {
      const data = await loadStatus();
      $('service-pill').textContent = data.service_online ? 'Service online' : 'Service offline';
      $('service-pill').className = 'pill ' + (data.service_online ? 'ok' : 'danger');
      $('status-cards').innerHTML = renderStatusCards(data);
      $('raw-json').textContent = JSON.stringify(data, null, 2);
      fillConfig(data);
    }
    async function refreshLogs() {
      const data = await loadLogs();
      const output = $('serial-logs');
      output.textContent = data.logs || '';
      output.scrollTop = output.scrollHeight;
      $('serial-log-meta').textContent = `${data.bytes || 0}/${data.max_bytes || 0} bytes`;
    }
    function showPage(pageName) {
      const button = document.querySelector(`nav button[data-page="${pageName}"]`);
      const pageElement = $('page-' + pageName);
      if (!button || !pageElement) return;
      document.querySelectorAll('nav button').forEach((item) => item.classList.remove('active'));
      document.querySelectorAll('.page').forEach((item) => item.classList.remove('active'));
      button.classList.add('active');
      pageElement.classList.add('active');
    }
    function getInitialPage() {
      const pageName = window.location.pathname.replace('/', '');
      if (pageName === 'ota' || pageName === 'config' || pageName === 'logs') return pageName;
      return 'home';
    }
    document.querySelectorAll('nav button').forEach((button) => {
      button.addEventListener('click', () => {
        showPage(button.dataset.page);
      });
    });
    $('config-form').addEventListener('submit', async (event) => { event.preventDefault(); await postConfig(readConfig()); await refresh(); });
    $('ota-form').addEventListener('submit', (event) => {
      event.preventDefault();
      const file = $('firmware').files[0];
      const form = new FormData();
      const request = new XMLHttpRequest();
      form.append('firmware', file, file.name);
      request.upload.onprogress = (progress) => { if (progress.lengthComputable) $('ota-bar').style.width = Math.round((progress.loaded * 100) / progress.total) + '%'; };
      request.onload = () => { $('ota-status').textContent = request.status < 300 ? 'Firmware received. Restarting...' : request.responseText; };
      request.onerror = () => { $('ota-status').textContent = 'Upload could not be completed.'; };
      request.open('POST', '/ota/upload');
      request.send(form);
    });
    showPage(getInitialPage());
    refresh().catch((error) => { $('service-pill').textContent = 'Error'; $('service-pill').className = 'pill danger'; $('raw-json').textContent = String(error); });
    refreshLogs().catch(() => {});
    setInterval(() => refresh().catch(() => {}), 5000);
    setInterval(() => refreshLogs().catch(() => {}), 2000);
  </script>
</body>
</html>)HTML";
}

void pushBootLogLine(String lines[], uint8_t &lineCount, const uint8_t maxLines, const String &line)
{
    if (line.length() == 0)
    {
        return;
    }

    if (lineCount < maxLines)
    {
        lines[lineCount] = line;
        lineCount++;
        return;
    }

    for (uint8_t i = 1; i < maxLines; i++)
    {
        lines[i - 1] = lines[i];
    }
    lines[maxLines - 1] = line;
}

// Splits the log buffer into up to maxLines lines wrapped at maxChars; returns how many there are.
uint8_t buildBootLogLines(String lines[], const uint8_t maxLines, const uint8_t maxChars)
{
    String currentLine = "";
    uint8_t lineCount = 0;

    for (size_t i = 0; i < serialLogBuffer.length(); i++)
    {
        const char character = serialLogBuffer.charAt(i);
        if (character == '\r')
        {
            continue;
        }
        if (character == '\n')
        {
            pushBootLogLine(lines, lineCount, maxLines, currentLine);
            currentLine = "";
            continue;
        }

        currentLine += character;
        if (currentLine.length() >= maxChars)
        {
            pushBootLogLine(lines, lineCount, maxLines, currentLine);
            currentLine = "";
        }
    }

    pushBootLogLine(lines, lineCount, maxLines, currentLine);
    return lineCount;
}

void drawMascotClaude(Adafruit_SSD1306 &display, const int16_t cx, const int16_t cy)
{
    // Claude "Spark": 12 radial rays from the center (starburst style).
    const float innerRadius = 2.5;
    const float outerRadius = 11.0;
    for (uint8_t i = 0; i < 12; i++)
    {
        const float angle = (float)i * (PI / 6.0);
        const int16_t x0 = cx + (int16_t)round(cos(angle) * innerRadius);
        const int16_t y0 = cy + (int16_t)round(sin(angle) * innerRadius);
        const int16_t x1 = cx + (int16_t)round(cos(angle) * outerRadius);
        const int16_t y1 = cy + (int16_t)round(sin(angle) * outerRadius);
        display.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
    }
    display.fillCircle(cx, cy, 2, SSD1306_WHITE);
}

void drawMascotCodex(Adafruit_SSD1306 &display, const int16_t cx, const int16_t cy)
{
    // Codex: terminal window with the ">_" prompt (same visual language as the web page).
    const int16_t w = 28;
    const int16_t h = 22;
    const int16_t x = cx - w / 2;
    const int16_t y = cy - h / 2;
    display.drawRoundRect(x, y, w, h, 3, SSD1306_WHITE);
    display.drawLine(x + 1, y + 6, x + w - 2, y + 6, SSD1306_WHITE);
    display.fillCircle(x + 4, y + 3, 1, SSD1306_WHITE);
    display.fillCircle(x + 8, y + 3, 1, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x + 5, y + 11);
    display.print(">_");
}

void drawBootLogContent(Adafruit_SSD1306 &display, const bool isClaude)
{
    String lines[BOOT_LOG_MAX_LINES];
    const uint8_t lineCount = buildBootLogLines(lines, BOOT_LOG_LINE_COUNT, BOOT_LOG_LINE_CHARS);

    display.clearDisplay();
    // White pixel frame around the screen edge.
    display.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);

    // Mascot + tool name in the left column.
    if (isClaude)
    {
        drawMascotClaude(display, BOOT_MASCOT_CX, BOOT_MASCOT_CY);
    }
    else
    {
        drawMascotCodex(display, BOOT_MASCOT_CX, BOOT_MASCOT_CY);
    }
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    const char *name = isClaude ? "Claude" : "Codex";
    const int16_t nameWidth = (int16_t)strlen(name) * 6;
    display.setCursor(BOOT_MASCOT_CX - nameWidth / 2, 44);
    display.print(name);

    // Vertical divider between the mascot and the logs.
    display.drawLine(BOOT_DIVIDER_X, 3, BOOT_DIVIDER_X, OLED_HEIGHT - 4, SSD1306_WHITE);

    // Logs in the right column, inside the frame.
    for (uint8_t i = 0; i < lineCount; i++)
    {
        display.setCursor(BOOT_LOG_X, BOOT_LOG_TOP + i * 8);
        display.print(lines[i]);
    }
}

void drawBootLogs(Adafruit_SSD1306 &display, const bool isClaude)
{
    drawBootLogContent(display, isClaude);
    display.display();
}

void drawBootLogDisplays()
{
    if (!liveLogDisplayEnabled)
    {
        return;
    }
    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        drawBootLogs(claudeOled, true);
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        drawBootLogs(codexOled, false);
    }
}

void drawEmergencyWifiContent(Adafruit_SSD1306 &display)
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Emergency WiFi");
    display.setCursor(0, 10);
    display.print("SSID:");
    display.setCursor(0, 20);
    display.print(EMERGENCY_AP_SSID);
    display.setCursor(0, 30);
    display.print("Password:");
    display.setCursor(0, 40);
    display.print(EMERGENCY_AP_PASSWORD);
    display.setCursor(0, 54);
    display.print(EMERGENCY_AP_URL);
}

void drawEmergencyWifiDisplays()
{
    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        drawEmergencyWifiContent(claudeOled);
        claudeOled.display();
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        drawEmergencyWifiContent(codexOled);
        codexOled.display();
    }
}

void initializeDisplays()
{
    claudeDisplayReady = false;
    codexDisplayReady = false;
    // The configured full contrast is applied; brightness returns to 100%.
    currentBrightnessPct = 100;

    if (settings.claudeDisplay.enabled)
    {
        claudeWire.begin(settings.claudeDisplay.sdaPin, settings.claudeDisplay.sclPin);
        claudeDisplayReady = claudeOled.begin(SSD1306_SWITCHCAPVCC, settings.claudeDisplay.address);
        if (claudeDisplayReady)
        {
            claudeOled.ssd1306_command(SSD1306_SETCONTRAST);
            claudeOled.ssd1306_command(settings.claudeDisplay.contrast);
        }
    }

    if (settings.codexDisplay.enabled)
    {
        codexWire.begin(settings.codexDisplay.sdaPin, settings.codexDisplay.sclPin);
        codexDisplayReady = codexOled.begin(SSD1306_SWITCHCAPVCC, settings.codexDisplay.address);
        if (codexDisplayReady)
        {
            codexOled.ssd1306_command(SSD1306_SETCONTRAST);
            codexOled.ssd1306_command(settings.codexDisplay.contrast);
        }
    }

    if (liveLogDisplayEnabled)
    {
        drawBootLogDisplays();
        return;
    }

    drawOfflineDisplays(String("Displays reinitialized without a new frame."));
}

void renderFrames()
{
    liveLogDisplayEnabled = false;
    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        claudeOled.clearDisplay();
        claudeOled.drawBitmap(0, 0, claudeFrame, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        claudeOled.display();
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        codexOled.clearDisplay();
        codexOled.drawBitmap(0, 0, codexFrame, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        codexOled.display();
    }
}

void drawWaitingOverlay(Adafruit_SSD1306 &display)
{
    // Animated ellipsis in the top-right corner: 0..3 dots per cycle. It stays INSIDE
    // the frame (does not touch the top edge y=0 or the right edge x=OLED_WIDTH-1) so it is
    // not clipped at boot; the clear starts at y=1 and ends 1px before the right edge.
    display.fillRect(108, 1, OLED_WIDTH - 108 - 1, 8, SSD1306_BLACK);
    const uint8_t dots = ellipsisStep % 4;
    for (uint8_t i = 0; i < dots; i++)
    {
        display.fillCircle(112 + i * 5, 5, 1, SSD1306_WHITE);
    }
}

void drawOfflineDisplays(const String &reason)
{
    // While the service is down for a short time the ellipsis animates; past the
    // grace period, the loop shows the screensaver and nothing is drawn here.
    logPrint(String("Ellipsis mode: "));
    logPrintln(reason);

    if (saverRunning)
    {
        return;
    }

    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        claudeOled.clearDisplay();
        if (framesReceived)
        {
            claudeOled.drawBitmap(0, 0, claudeFrame, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        }
        else
        {
            drawBootLogContent(claudeOled, true);
        }
        drawWaitingOverlay(claudeOled);
        claudeOled.display();
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        codexOled.clearDisplay();
        if (framesReceived)
        {
            codexOled.drawBitmap(0, 0, codexFrame, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        }
        else
        {
            drawBootLogContent(codexOled, false);
        }
        drawWaitingOverlay(codexOled);
        codexOled.display();
    }
    ellipsisStep++;
}

String httpHeaderValue(const String &headerBlock, const String &lowerBlock, const char *name)
{
    // Returns the value of "<name>:" in the header block (case-insensitive) or "".
    // lowerBlock is the block already lowercased (the caller computes it ONCE and reuses it for
    // all headers, instead of copying+lowercasing the block on every lookup).
    String needle = String(name);
    needle.toLowerCase();
    needle += ":";
    const int idx = lowerBlock.indexOf(needle);
    if (idx < 0)
    {
        return "";
    }
    const int valueStart = idx + needle.length();
    int lineEnd = headerBlock.indexOf("\r\n", valueStart);
    if (lineEnd < 0)
    {
        lineEnd = headerBlock.length();
    }
    String value = headerBlock.substring(valueStart, lineEnd);
    value.trim();
    return value;
}

void applyFramesHeaders(const String &headerBlock, const String &lowerBlock)
{
    // Activity, animation, screensaver and brightness arrive in headers (200 and 304).
    const String headerClaudeActivity = httpHeaderValue(headerBlock, lowerBlock, "X-Act-Claude");
    const String headerCodexActivity = httpHeaderValue(headerBlock, lowerBlock, "X-Act-Codex");
    const String headerAnimEtag = httpHeaderValue(headerBlock, lowerBlock, "X-Anim-Etag");
    const String headerSaver = httpHeaderValue(headerBlock, lowerBlock, "X-Saver");
    if (headerClaudeActivity.length() > 0)
    {
        claudeContentActivity = headerClaudeActivity;
    }
    if (headerCodexActivity.length() > 0)
    {
        codexContentActivity = headerCodexActivity;
    }
    // Busy sessions in parallel (2 dots per session on the ESP). Clamped to [0, 8] so the
    // box does not overflow; during busy the firmware guarantees at least 1 (2 dots).
    const String headerSessionsClaude = httpHeaderValue(headerBlock, lowerBlock, "X-Sessions-Claude");
    const String headerSessionsCodex = httpHeaderValue(headerBlock, lowerBlock, "X-Sessions-Codex");
    if (headerSessionsClaude.length() > 0)
    {
        claudeBusySessions = static_cast<uint8_t>(constrain(headerSessionsClaude.toInt(), 0, 8));
    }
    if (headerSessionsCodex.length() > 0)
    {
        codexBusySessions = static_cast<uint8_t>(constrain(headerSessionsCodex.toInt(), 0, 8));
    }
    if (headerAnimEtag.length() > 0)
    {
        pendingAnimEtag = headerAnimEtag;
    }
    const String headerAnimStyle = httpHeaderValue(headerBlock, lowerBlock, "X-Anim-Style");
    if (headerAnimStyle.length() > 0)
    {
        activityStyle = headerAnimStyle;
    }
    if (headerSaver.length() > 0)
    {
        // Learned while there is service and persisted to use it offline.
        const uint8_t nextSaver = parseSaverId(headerSaver);
        if (nextSaver != currentSaverId)
        {
            currentSaverId = nextSaver;
            preferences.begin("monitor", false);
            preferences.putUChar("saver", currentSaverId);
            preferences.end();
        }
    }
    const String headerBrightness = httpHeaderValue(headerBlock, lowerBlock, "X-Brightness");
    if (headerBrightness.length() > 0 && !saverRunning)
    {
        applyBrightness(static_cast<uint8_t>(constrain(headerBrightness.toInt(), 0, 100)));
    }
}

void startFramesFetch()
{
    String host = "";
    uint16_t port = 0;
    if (!parseServiceHostPort(settings.serviceUrl, host, port))
    {
        monitorState.serviceOnline = false;
        monitorState.serviceError = "Invalid service URL";
        drawOfflineDisplays(String("Invalid service URL: ") + settings.serviceUrl);
        return;
    }

    if (!framesClient.connect(host.c_str(), port, FRAMES_CONNECT_TIMEOUT_MS))
    {
        framesClient.stop();
        monitorState.serviceOnline = false;
        monitorState.serviceError = "Could not connect";
        drawOfflineDisplays(String("Could not connect host=") + host + String(":") + String(port));
        return;
    }

    // Raw request: the If-None-Match enables the 304 (do not redraw if nothing changed) and the
    // server sends X-Act-* / ETag in both 200 and 304.
    String request = String("GET /api/esp/frames HTTP/1.1\r\n") +
                     String("Host: ") + host + String("\r\n");
    if (frameEtag.length() > 0)
    {
        request += String("If-None-Match: ") + frameEtag + String("\r\n");
    }
    request += String("Connection: close\r\n\r\n");
    framesClient.print(request);

    framesRespLen = 0;
    framesFetchActive = true;
    framesFetchStartMs = millis();
}

void processFramesResponse()
{
    // Separates headers (text) from body (binary) at the first "\r\n\r\n".
    int boundary = -1;
    for (size_t i = 0; i + 3 < framesRespLen; i++)
    {
        if (framesRespBuf[i] == '\r' && framesRespBuf[i + 1] == '\n' &&
            framesRespBuf[i + 2] == '\r' && framesRespBuf[i + 3] == '\n')
        {
            boundary = static_cast<int>(i);
            break;
        }
    }
    if (boundary < 0)
    {
        monitorState.serviceOnline = false;
        monitorState.serviceError = "Response without headers";
        drawOfflineDisplays(String("Frames response without headers"));
        return;
    }

    String headerBlock = "";
    headerBlock.reserve(boundary + 1);
    for (int i = 0; i < boundary; i++)
    {
        headerBlock += static_cast<char>(framesRespBuf[i]);
    }

    // Status code from the first line: "HTTP/1.1 200 OK".
    int statusCode = 0;
    const int firstSpace = headerBlock.indexOf(' ');
    if (firstSpace >= 0)
    {
        statusCode = headerBlock.substring(firstSpace + 1, firstSpace + 4).toInt();
    }

    // Lowercased block once: shared by all header lookups.
    String lowerBlock = headerBlock;
    lowerBlock.toLowerCase();
    applyFramesHeaders(headerBlock, lowerBlock);
    const String etag = httpHeaderValue(headerBlock, lowerBlock, "ETag");
    if (etag.length() > 0)
    {
        frameEtag = etag;
    }

    if (statusCode == 304)
    {
        if (!framesReceived)
        {
            monitorState.serviceOnline = false;
            monitorState.serviceError = "HTTP 304 without a previous frame";
            drawOfflineDisplays(String("HTTP 304 without a previous frame"));
            return;
        }
        // No changes since the last frame: nothing is redrawn and no ellipsis is shown.
        if (millis() - lastNoChangeLogAtMs >= PERIODIC_STATUS_LOG_INTERVAL_MS)
        {
            logPrintln(String("Polling frames unchanged: HTTP 304"));
            lastNoChangeLogAtMs = millis();
        }
        monitorState.serviceOnline = true;
        monitorState.serviceError = "";
        monitorState.lastSuccessMs = millis();
        return;
    }

    if (statusCode != 200)
    {
        monitorState.serviceOnline = false;
        monitorState.serviceError = String("HTTP ") + String(statusCode);
        drawOfflineDisplays(String("Polling failed status=") + String(statusCode));
        return;
    }

    const size_t bodyStart = static_cast<size_t>(boundary) + 4;
    const size_t bodyLen = framesRespLen - bodyStart;
    if (bodyLen != FRAMES_TOTAL_BYTES)
    {
        monitorState.serviceOnline = false;
        monitorState.serviceError = String("Incomplete frame: ") + String(bodyLen);
        drawOfflineDisplays(String("Incomplete frame bytes=") + String(bodyLen) + String(" expected=") + String(FRAMES_TOTAL_BYTES));
        return;
    }

    // Swapped displays: the Claude OLED (21/22) shows the Codex frame
    // and the Codex OLED (16/17) shows the Claude frame.
    const bool wasServiceOnline = monitorState.serviceOnline;
    const bool hadFrame = framesReceived;
    memcpy(claudeFrame, framesRespBuf + bodyStart + FRAME_BYTES, FRAME_BYTES);
    memcpy(codexFrame, framesRespBuf + bodyStart, FRAME_BYTES);
    framesReceived = true;
    monitorState.serviceOnline = true;
    monitorState.serviceError = "";
    monitorState.lastSuccessMs = millis();
    monitorState.frameUpdatedAtMs = millis();
    if (!hadFrame || !wasServiceOnline)
    {
        logPrint(String("Frame received OK bytes="));
        logPrint(String(FRAMES_TOTAL_BYTES));
        logPrint(String(" etag="));
        logPrintln(frameEtag);
    }
    renderFrames();
}

void serviceFramesFetch()
{
    // Non-blocking state machine: starts a fetch every pollInterval and reads it
    // in chunks on each loop pass, without freezing the render while the server responds.
    if (emergencyWifiActive)
    {
        if (framesFetchActive)
        {
            framesClient.stop();
            framesFetchActive = false;
        }
        return;
    }

    if (!framesFetchActive)
    {
        if (millis() - monitorState.lastPollMs >= settings.pollIntervalMs)
        {
            monitorState.lastPollMs = millis();
            startFramesFetch();
        }
        return;
    }

    // Safety cap: if the server did not respond in time, cut and retry later.
    if (millis() - framesFetchStartMs > FRAMES_FETCH_TIMEOUT_MS)
    {
        framesClient.stop();
        framesFetchActive = false;
        monitorState.serviceOnline = false;
        monitorState.serviceError = "Frames timeout";
        drawOfflineDisplays(String("Timeout waiting for frames"));
        return;
    }

    // Non-blocking read: consumes in bulk whatever is available on this loop pass
    // (readBytes with the available count does not block and is more efficient than byte by byte).
    const int available = framesClient.available();
    if (available > 0 && framesRespLen < FRAMES_RESP_MAX_BYTES)
    {
        const size_t room = FRAMES_RESP_MAX_BYTES - framesRespLen;
        size_t toRead = static_cast<size_t>(available);
        if (toRead > room)
        {
            toRead = room;
        }
        framesRespLen += framesClient.readBytes(framesRespBuf + framesRespLen, toRead);
    }

    // Response larger than expected (should not happen): abort to avoid corruption.
    if (framesRespLen >= FRAMES_RESP_MAX_BYTES && framesClient.available() > 0)
    {
        framesClient.stop();
        framesFetchActive = false;
        monitorState.serviceOnline = false;
        monitorState.serviceError = "Response too large";
        return;
    }

    // The server closes when done (Connection: close): response complete.
    if (!framesClient.connected() && framesClient.available() == 0)
    {
        framesClient.stop();
        framesFetchActive = false;
        processFramesResponse();
    }
}

void parseActivityAnimation(const uint8_t *data, const size_t length, const String &etag)
{
    if (length < ANIM_PACK_HEADER_BYTES)
    {
        logPrintln(String("Animation: packet too short"));
        return;
    }
    if (data[0] != ANIM_PACK_MAGIC || data[1] != ANIM_PACK_VERSION)
    {
        logPrintln(String("Animation: invalid header"));
        return;
    }

    const uint8_t frameW = data[2];
    const uint8_t frameH = data[3];
    const uint8_t frameCount = data[4];
    const uint8_t flags = data[5];
    const uint16_t intervalMs = static_cast<uint16_t>(data[6] | (data[7] << 8));
    const uint16_t blinkMs = static_cast<uint16_t>(data[8] | (data[9] << 8));
    const uint16_t frameBytes = static_cast<uint16_t>(((frameW + 7) / 8) * frameH);
    const size_t bodyBytes = static_cast<size_t>(frameCount) * frameBytes;

    if (frameW == 0 || frameH == 0 || frameW > ACTIVITY_BOX_MAX_W || frameH > ACTIVITY_BOX_MAX_H ||
        frameBytes == 0 || frameCount == 0 || frameCount > ANIM_MAX_FRAMES || bodyBytes > sizeof(animFrameData) ||
        (ANIM_PACK_HEADER_BYTES + bodyBytes) > length)
    {
        logPrintln(String("Animation: dimensions out of range"));
        return;
    }

    memcpy(animFrameData, data + ANIM_PACK_HEADER_BYTES, bodyBytes);
    activityAnimation.frameW = frameW;
    activityAnimation.frameH = frameH;
    activityAnimation.frameCount = frameCount;
    activityAnimation.frameBytes = frameBytes;
    activityAnimation.intervalMs = intervalMs >= 20 ? intervalMs : 20;
    activityAnimation.blinkMs = blinkMs >= 100 ? blinkMs : 100;
    activityAnimation.invertOnWaiting = (flags & 0x01) != 0;
    activityAnimation.valid = true;
    activityAnimationEtag = etag;

    claudeRuntime.animActive = false;
    codexRuntime.animActive = false;

    logPrint(String("Animation stored frames="));
    logPrint(String(frameCount));
    logPrint(String(" ") + String(frameW) + String("x") + String(frameH));
    logPrint(String(" interval=") + String(activityAnimation.intervalMs));
    logPrintln(String("ms"));
}

void fetchActivityAnimation()
{
    HTTPClient httpClient;
    const String endpoint = settings.serviceUrl + String("/api/esp/activity-animation");
    if (!httpClient.begin(endpoint))
    {
        logPrintln(String("Animation: could not start HTTP"));
        return;
    }

    httpClient.setTimeout(HTTP_CLIENT_TIMEOUT_MS);
    httpClient.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    if (activityAnimationEtag.length() > 0)
    {
        httpClient.addHeader("If-None-Match", activityAnimationEtag);
    }
    const char *headerKeys[] = {"ETag"};
    httpClient.collectHeaders(headerKeys, 1);

    const int statusCode = httpClient.GET();
    if (statusCode == HTTP_CODE_NOT_MODIFIED)
    {
        httpClient.end();
        return;
    }
    if (statusCode != HTTP_CODE_OK)
    {
        logPrint(String("Animation: download failed status="));
        logPrintln(String(statusCode));
        httpClient.end();
        return;
    }

    const int contentLength = httpClient.getSize();
    static uint8_t downloadBuffer[ANIM_MAX_BYTES];
    if (contentLength > static_cast<int>(sizeof(downloadBuffer)))
    {
        logPrint(String("Animation: packet too large bytes="));
        logPrintln(String(contentLength));
        httpClient.end();
        return;
    }

    size_t want = sizeof(downloadBuffer);
    if (contentLength > 0 && static_cast<size_t>(contentLength) < want)
    {
        want = static_cast<size_t>(contentLength);
    }

    WiFiClient *stream = httpClient.getStreamPtr();
    size_t readTotal = 0;
    const uint32_t startMs = millis();
    while (readTotal < want && (millis() - startMs) < 4000)
    {
        if (stream->available() > 0)
        {
            readTotal += stream->readBytes(downloadBuffer + readTotal, want - readTotal);
        }
        else
        {
            delay(1);
        }
    }

    const String newEtag = httpClient.header("ETag");
    httpClient.end();

    parseActivityAnimation(downloadBuffer, readTotal, newEtag);
}

bool parseServiceHostPort(const String &url, String &host, uint16_t &port)
{
    // Extracts host and port from "http://host:port[/...]" (no TLS support).
    if (!url.startsWith("http://"))
    {
        return false;
    }
    String authority = url.substring(7);
    const int slashIndex = authority.indexOf('/');
    if (slashIndex >= 0)
    {
        authority = authority.substring(0, slashIndex);
    }
    const int colonIndex = authority.indexOf(':');
    if (colonIndex >= 0)
    {
        host = authority.substring(0, colonIndex);
        port = static_cast<uint16_t>(authority.substring(colonIndex + 1).toInt());
    }
    else
    {
        host = authority;
        port = 80;
    }
    return host.length() > 0 && port > 0;
}

void applyActivityResponse(const String &response)
{
    // The body is "<claude> <codex>" after the blank header line.
    const int bodyIndex = response.indexOf("\r\n\r\n");
    if (bodyIndex < 0)
    {
        return;
    }
    String body = response.substring(bodyIndex + 4);
    body.trim();
    const int spaceIndex = body.indexOf(' ');
    if (spaceIndex < 0)
    {
        return;
    }
    const String nextClaude = body.substring(0, spaceIndex);
    const String nextCodex = body.substring(spaceIndex + 1);
    if (nextClaude.length() > 0)
    {
        claudeContentActivity = nextClaude;
    }
    if (nextCodex.length() > 0)
    {
        codexContentActivity = nextCodex;
    }
}

void startActivityLongPoll()
{
    String host = "";
    uint16_t port = 0;
    if (!parseServiceHostPort(settings.serviceUrl, host, port))
    {
        activityPollNextStartMs = millis() + ACTIVITY_LONGPOLL_RETRY_MS;
        return;
    }

    if (!activityClient.connect(host.c_str(), port, ACTIVITY_CONNECT_TIMEOUT_MS))
    {
        activityClient.stop();
        activityPollNextStartMs = millis() + ACTIVITY_LONGPOLL_RETRY_MS;
        return;
    }

    // Sends the known state: the server holds until it differs or the cap expires.
    const String request = String("GET /api/esp/activity-wait?c=") + claudeContentActivity +
                           String("&x=") + codexContentActivity + String(" HTTP/1.1\r\n") +
                           String("Host: ") + host + String("\r\n") +
                           String("Connection: close\r\n\r\n");
    activityClient.print(request);
    activityResponse = "";
    activityPollActive = true;
    activityPollStartMs = millis();
}

void stopActivityLongPoll(uint32_t retryAtMs)
{
    // Closes the socket and discards the partial response; avoids leaving clients alive when
    // switching to AP mode or after long-poll timeouts.
    activityClient.stop();
    activityPollActive = false;
    activityResponse = "";
    activityPollNextStartMs = retryAtMs;
}

void serviceActivityLongPoll()
{
    if (emergencyWifiActive)
    {
        stopActivityLongPoll(millis() + ACTIVITY_LONGPOLL_RETRY_MS);
        return;
    }

    // With the screensaver active the service is down: activity is
    // irrelevant (the screen shows the animation) and the blocking connect() of the
    // long-poll to an unreachable host would freeze the render. pollFrames keeps
    // polling and removes the screensaver as soon as the service returns.
    if (saverRunning)
    {
        stopActivityLongPoll(millis() + ACTIVITY_LONGPOLL_RETRY_MS);
        return;
    }

    if (!activityPollActive)
    {
        if (millis() >= activityPollNextStartMs)
        {
            startActivityLongPoll();
        }
        return;
    }

    // Safety cap: if the server did not respond, cut and retry.
    if (millis() - activityPollStartMs > ACTIVITY_LONGPOLL_TIMEOUT_MS)
    {
        stopActivityLongPoll(millis());
        return;
    }

    // Non-blocking read: only consumes what is available on this loop pass.
    while (activityClient.available() > 0)
    {
        activityResponse += static_cast<char>(activityClient.read());
        if (activityResponse.length() > ACTIVITY_RESPONSE_MAX_BYTES)
        {
            activityResponse.remove(0, activityResponse.length() - ACTIVITY_RESPONSE_MAX_BYTES);
        }
    }

    // The server closes when done (Connection: close): response complete.
    if (!activityClient.connected() && activityClient.available() == 0)
    {
        applyActivityResponse(activityResponse);
        stopActivityLongPoll(millis());
    }
}

// ---- Screensaver (anti burn-in) -------------------------------------------
const char SAVER_MATRIX_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

uint32_t saverRand(SaverState &state)
{
    state.rng = state.rng * 1664525u + 1013904223u;
    return state.rng;
}

uint8_t parseSaverId(const String &name)
{
    if (name == "black") return SAVER_BLACK;
    if (name == "snake") return SAVER_SNAKE;
    if (name == "pipes") return SAVER_PIPES;
    if (name == "matrix") return SAVER_MATRIX;
    if (name == "dvd") return SAVER_DVD;
    if (name == "maze") return SAVER_MAZE;
    if (name == "flower") return SAVER_FLOWER;
    return SAVER_DEFAULT;
}

void mazeGenerate(SaverState &state)
{
    // Perfect maze via iterative backtracker over 16x8 cells. Each cell
    // stores whether it keeps its East and South wall (bitmask); they are removed when connecting.
    for (uint8_t i = 0; i < 16; i++)
    {
        state.mazeWallE[i] = 0xFF;
        state.mazeWallS[i] = 0xFF;
    }
    uint8_t visited[16] = {0};
    uint8_t stack[128];
    uint8_t sp = 0;
    uint8_t current = 0;
    visited[0] |= 1;
    stack[sp++] = 0;

    while (sp > 0)
    {
        current = stack[sp - 1];
        const uint8_t cx = current % 16;
        const uint8_t cy = current / 16;
        uint8_t candidate[4];
        uint8_t direction[4];
        uint8_t count = 0;
        if (cx + 1 < 16 && !((visited[(current + 1) >> 3] >> ((current + 1) & 7)) & 1)) { candidate[count] = current + 1; direction[count] = 0; count++; }
        if (cx > 0 && !((visited[(current - 1) >> 3] >> ((current - 1) & 7)) & 1)) { candidate[count] = current - 1; direction[count] = 1; count++; }
        if (cy + 1 < 8 && !((visited[(current + 16) >> 3] >> ((current + 16) & 7)) & 1)) { candidate[count] = current + 16; direction[count] = 2; count++; }
        if (cy > 0 && !((visited[(current - 16) >> 3] >> ((current - 16) & 7)) & 1)) { candidate[count] = current - 16; direction[count] = 3; count++; }

        if (count == 0)
        {
            sp--;
            continue;
        }
        const uint8_t pick = saverRand(state) % count;
        const uint8_t next = candidate[pick];
        switch (direction[pick])
        {
            case 0: state.mazeWallE[current >> 3] &= ~(1 << (current & 7)); break;
            case 1: state.mazeWallE[next >> 3] &= ~(1 << (next & 7)); break;
            case 2: state.mazeWallS[current >> 3] &= ~(1 << (current & 7)); break;
            default: state.mazeWallS[next >> 3] &= ~(1 << (next & 7)); break;
        }
        visited[next >> 3] |= (1 << (next & 7));
        stack[sp++] = next;
    }
    state.mazeX = 0;
    state.mazeY = 0;
}

void initSaver(SaverState &state, uint32_t seed)
{
    state.initialized = true;
    state.frame = 0;
    state.rng = seed | 1u;
    state.dvdX = 12;
    state.dvdY = 10;
    state.dvdVX = 2;
    state.dvdVY = 1;
    state.snakeLen = 5;
    for (uint8_t i = 0; i < state.snakeLen; i++)
    {
        state.snakeX[i] = 16;
        state.snakeY[i] = 8;
    }
    state.foodX = saverRand(state) % 32;
    state.foodY = saverRand(state) % 16;
    for (uint8_t i = 0; i < 22; i++)
    {
        state.drop[i] = -static_cast<int16_t>(saverRand(state) % OLED_HEIGHT);
        state.dropSpeed[i] = 2 + (saverRand(state) % 4);
    }
    state.pipeX = 8;
    state.pipeY = 4;
    state.pipeDir = 0;
    mazeGenerate(state);
}

void drawDvdEllipse(Adafruit_SSD1306 &display, const int16_t cx, const int16_t cy, const int16_t rx, const int16_t ry)
{
    // Ellipse outline by segments (Adafruit GFX does not include drawEllipse).
    const uint8_t steps = 24;
    int16_t px = cx + rx;
    int16_t py = cy;
    for (uint8_t i = 1; i <= steps; i++)
    {
        const float angle = (float)i / steps * 2.0f * PI;
        const int16_t nx = cx + (int16_t)round(rx * cosf(angle));
        const int16_t ny = cy + (int16_t)round(ry * sinf(angle));
        display.drawLine(px, py, nx, ny, SSD1306_WHITE);
        px = nx;
        py = ny;
    }
}

void drawSaverDvd(Adafruit_SSD1306 &display, SaverState &state)
{
    const int16_t w = 40;
    const int16_t h = 22;
    // Constant integer speed (2/1 px per frame): smooth, without frame jumps.
    state.dvdX += state.dvdVX;
    state.dvdY += state.dvdVY;
    if (state.dvdX <= 0 || state.dvdX + w >= OLED_WIDTH) { state.dvdVX = -state.dvdVX; state.dvdX = constrain(state.dvdX, 0, OLED_WIDTH - w); }
    if (state.dvdY <= 0 || state.dvdY + h >= OLED_HEIGHT) { state.dvdVY = -state.dvdVY; state.dvdY = constrain(state.dvdY, 0, OLED_HEIGHT - h); }
    display.clearDisplay();
    const int16_t cx = state.dvdX + w / 2;
    // Mini DVD logo: characteristic oval + "DVD" inside + "VIDEO" band below.
    drawDvdEllipse(display, cx, state.dvdY + 7, 18, 6);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(cx - 9, state.dvdY + 4);
    display.print("DVD");
    display.fillRoundRect(state.dvdX + 4, state.dvdY + 14, 32, 8, 2, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(state.dvdX + 6, state.dvdY + 15);
    display.print("VIDEO");
    display.display();
}

void drawSaverSnake(Adafruit_SSD1306 &display, SaverState &state)
{
    const uint8_t hx = state.snakeX[state.snakeLen - 1];
    const uint8_t hy = state.snakeY[state.snakeLen - 1];
    int8_t sx = (state.foodX > hx) ? 1 : ((state.foodX < hx) ? -1 : 0);
    int8_t sy = (state.foodY > hy) ? 1 : ((state.foodY < hy) ? -1 : 0);
    if (sx != 0 && sy != 0)
    {
        if (saverRand(state) & 1) { sy = 0; } else { sx = 0; }
    }
    const uint8_t nx = (hx + sx + 32) % 32;
    const uint8_t ny = (hy + sy + 16) % 16;
    const bool eat = (nx == state.foodX && ny == state.foodY);
    if (eat && state.snakeLen < 48)
    {
        state.snakeX[state.snakeLen] = nx;
        state.snakeY[state.snakeLen] = ny;
        state.snakeLen++;
    }
    else
    {
        for (uint8_t i = 1; i < state.snakeLen; i++)
        {
            state.snakeX[i - 1] = state.snakeX[i];
            state.snakeY[i - 1] = state.snakeY[i];
        }
        state.snakeX[state.snakeLen - 1] = nx;
        state.snakeY[state.snakeLen - 1] = ny;
    }
    if (eat)
    {
        state.foodX = saverRand(state) % 32;
        state.foodY = saverRand(state) % 16;
    }
    display.clearDisplay();
    display.fillRect(state.foodX * 4 + 1, state.foodY * 4 + 1, 2, 2, SSD1306_WHITE);
    for (uint8_t i = 0; i < state.snakeLen; i++)
    {
        display.fillRect(state.snakeX[i] * 4, state.snakeY[i] * 4, 3, 3, SSD1306_WHITE);
    }
    display.display();
}

void drawSaverMatrix(Adafruit_SSD1306 &display, SaverState &state)
{
    const uint8_t colWidth = 10;
    const uint8_t rowHeight = 11;
    const uint8_t columns = OLED_WIDTH / colWidth;
    const uint8_t charCount = sizeof(SAVER_MATRIX_CHARS) - 1;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    for (uint8_t c = 0; c < columns; c++)
    {
        const int16_t head = state.drop[c];
        const int16_t x = c * colWidth;
        for (uint8_t t = 0; t < 6; t++)
        {
            const int16_t y = head - t * rowHeight;
            if (y >= 0 && y < OLED_HEIGHT)
            {
                display.setCursor(x, y);
                display.write(SAVER_MATRIX_CHARS[saverRand(state) % charCount]);
            }
        }
        state.drop[c] += state.dropSpeed[c];
        if (state.drop[c] - 6 * rowHeight > OLED_HEIGHT)
        {
            state.drop[c] = -static_cast<int16_t>(saverRand(state) % 32);
            state.dropSpeed[c] = 2 + (saverRand(state) % 4);
        }
    }
    display.display();
}

void drawSaverPipes(Adafruit_SSD1306 &display, SaverState &state)
{
    const uint8_t cell = 8;
    const uint8_t cols = OLED_WIDTH / cell;
    const uint8_t rows = OLED_HEIGHT / cell;
    if (state.frame % 300 == 0)
    {
        display.clearDisplay();
    }
    if (saverRand(state) % 100 < 30)
    {
        state.pipeDir = (state.pipeDir + ((saverRand(state) & 1) ? 1 : 3)) % 4;
    }
    const int8_t dx[4] = {1, 0, -1, 0};
    const int8_t dy[4] = {0, 1, 0, -1};
    int16_t nx = state.pipeX + dx[state.pipeDir];
    int16_t ny = state.pipeY + dy[state.pipeDir];
    if (nx < 0 || nx >= cols || ny < 0 || ny >= rows)
    {
        state.pipeDir = (state.pipeDir + 2) % 4;
        nx = constrain(state.pipeX + dx[state.pipeDir], 0, cols - 1);
        ny = constrain(state.pipeY + dy[state.pipeDir], 0, rows - 1);
    }
    state.pipeX = nx;
    state.pipeY = ny;
    display.drawCircle(state.pipeX * cell + cell / 2, state.pipeY * cell + cell / 2, 2, SSD1306_WHITE);
    display.display();
}

void drawSaverMaze(Adafruit_SSD1306 &display, SaverState &state)
{
    const uint8_t cell = 8;
    const uint8_t cols = 16;
    const uint8_t rows = 8;
    // The dot advances to a connected neighbor (no wall between cells).
    const uint8_t idx = state.mazeY * cols + state.mazeX;
    uint8_t toX[4];
    uint8_t toY[4];
    uint8_t count = 0;
    if (state.mazeX + 1 < cols && !((state.mazeWallE[idx >> 3] >> (idx & 7)) & 1)) { toX[count] = state.mazeX + 1; toY[count] = state.mazeY; count++; }
    if (state.mazeX > 0)
    {
        const uint8_t west = idx - 1;
        if (!((state.mazeWallE[west >> 3] >> (west & 7)) & 1)) { toX[count] = state.mazeX - 1; toY[count] = state.mazeY; count++; }
    }
    if (state.mazeY + 1 < rows && !((state.mazeWallS[idx >> 3] >> (idx & 7)) & 1)) { toX[count] = state.mazeX; toY[count] = state.mazeY + 1; count++; }
    if (state.mazeY > 0)
    {
        const uint8_t north = idx - cols;
        if (!((state.mazeWallS[north >> 3] >> (north & 7)) & 1)) { toX[count] = state.mazeX; toY[count] = state.mazeY - 1; count++; }
    }
    if (count > 0)
    {
        const uint8_t pick = saverRand(state) % count;
        state.mazeX = toX[pick];
        state.mazeY = toY[pick];
    }

    display.clearDisplay();
    for (uint8_t cy = 0; cy < rows; cy++)
    {
        for (uint8_t cx = 0; cx < cols; cx++)
        {
            const uint8_t cellIdx = cy * cols + cx;
            const int16_t x0 = cx * cell;
            const int16_t y0 = cy * cell;
            const int16_t x1 = x0 + cell - 1;
            const int16_t y1 = y0 + cell - 1;
            if (cx == 0) display.drawLine(x0, y0, x0, y1, SSD1306_WHITE);
            if (cy == 0) display.drawLine(x0, y0, x1, y0, SSD1306_WHITE);
            if ((state.mazeWallE[cellIdx >> 3] >> (cellIdx & 7)) & 1) display.drawLine(x1, y0, x1, y1, SSD1306_WHITE);
            if ((state.mazeWallS[cellIdx >> 3] >> (cellIdx & 7)) & 1) display.drawLine(x0, y1, x1, y1, SSD1306_WHITE);
        }
    }
    display.fillRect(state.mazeX * cell + 2, state.mazeY * cell + 2, cell - 4, cell - 4, SSD1306_WHITE);
    display.display();
}

void drawSaverFlower(Adafruit_SSD1306 &display, SaverState &state)
{
    const int16_t radius = 14;
    state.dvdX += state.dvdVX;
    state.dvdY += state.dvdVY;
    if (state.dvdX - radius <= 0 || state.dvdX + radius >= OLED_WIDTH) { state.dvdVX = -state.dvdVX; state.dvdX = constrain(state.dvdX, radius, OLED_WIDTH - radius); }
    if (state.dvdY - radius <= 0 || state.dvdY + radius >= OLED_HEIGHT) { state.dvdVY = -state.dvdVY; state.dvdY = constrain(state.dvdY, radius, OLED_HEIGHT - radius); }
    const float rotation = state.frame * 0.2f;
    display.clearDisplay();
    float prevX = 0;
    float prevY = 0;
    bool have = false;
    for (uint16_t i = 0; i <= 80; i++)
    {
        const float theta = (i / 80.0f) * 6.2831853f;
        const float r = radius * fabsf(cosf(5.0f * theta));
        const float x = state.dvdX + r * cosf(theta + rotation);
        const float y = state.dvdY + r * sinf(theta + rotation);
        if (have)
        {
            display.drawLine(static_cast<int16_t>(prevX), static_cast<int16_t>(prevY), static_cast<int16_t>(x), static_cast<int16_t>(y), SSD1306_WHITE);
        }
        prevX = x;
        prevY = y;
        have = true;
    }
    display.display();
}

void drawSaverFrame(Adafruit_SSD1306 &display, SaverState &state, uint8_t saverId)
{
    switch (saverId)
    {
        case SAVER_SNAKE: drawSaverSnake(display, state); break;
        case SAVER_PIPES: drawSaverPipes(display, state); break;
        case SAVER_MATRIX: drawSaverMatrix(display, state); break;
        case SAVER_DVD: drawSaverDvd(display, state); break;
        case SAVER_MAZE: drawSaverMaze(display, state); break;
        case SAVER_FLOWER: drawSaverFlower(display, state); break;
        default: display.clearDisplay(); display.display(); break;
    }
    state.frame++;
}

void applyBrightness(uint8_t pct)
{
    // Applies pct% of each screen's configured contrast. Only touches I2C if it changes.
    if (pct > 100)
    {
        pct = 100;
    }
    if (pct == currentBrightnessPct)
    {
        return;
    }
    currentBrightnessPct = pct;
    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        claudeOled.ssd1306_command(SSD1306_SETCONTRAST);
        claudeOled.ssd1306_command(static_cast<uint8_t>(settings.claudeDisplay.contrast * static_cast<uint16_t>(pct) / 100));
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        codexOled.ssd1306_command(SSD1306_SETCONTRAST);
        codexOled.ssd1306_command(static_cast<uint8_t>(settings.codexDisplay.contrast * static_cast<uint16_t>(pct) / 100));
    }
}

void resetActivityRuntimeForSaver(Adafruit_SSD1306 &display, DisplayActivityRuntime &runtime, const bool ready)
{
    if (!ready)
    {
        return;
    }

    // The screensaver must start in normal polarity; if we came from "waiting",
    // an inverted black screen would end up white and worse for burn-in.
    display.invertDisplay(false);
    runtime.invertOn = false;
    runtime.animActive = false;
    runtime.animIndex = 0;
    runtime.lastAnimStepMs = 0;
    runtime.lastInvertToggleMs = 0;
}

void startScreenSaver()
{
    // The screensaver is shown at full brightness (it already prevents burn-in on its own).
    applyBrightness(100);
    resetActivityRuntimeForSaver(claudeOled, claudeRuntime, settings.claudeDisplay.enabled && claudeDisplayReady);
    resetActivityRuntimeForSaver(codexOled, codexRuntime, settings.codexDisplay.enabled && codexDisplayReady);
    // Re-seeds each screen's state when the screensaver is activated.
    initSaver(claudeSaver, millis() ^ 0xA53Cu);
    initSaver(codexSaver, (millis() << 1) ^ 0x5AC3u);
}

void renderScreenSaver()
{
    if (settings.claudeDisplay.enabled && claudeDisplayReady)
    {
        drawSaverFrame(claudeOled, claudeSaver, currentSaverId);
    }
    if (settings.codexDisplay.enabled && codexDisplayReady)
    {
        drawSaverFrame(codexOled, codexSaver, currentSaverId);
    }
}

uint16_t activityBoxX()
{
    // Box anchored to the right edge: if the width changes, it stays stuck to the right.
    return (activityAnimation.frameW <= OLED_WIDTH) ? (OLED_WIDTH - activityAnimation.frameW) : 0;
}

void clearActivityBox(Adafruit_SSD1306 &display)
{
    display.fillRect(activityBoxX(), ACTIVITY_BOX_Y, activityAnimation.frameW, activityAnimation.frameH, SSD1306_BLACK);
}

void drawActivityFrame(Adafruit_SSD1306 &display, const uint8_t index)
{
    clearActivityBox(display);
    if (!activityAnimation.valid || index >= activityAnimation.frameCount)
    {
        return;
    }

    const uint8_t *framePtr = animFrameData + static_cast<size_t>(index) * activityAnimation.frameBytes;
    display.drawBitmap(activityBoxX(), ACTIVITY_BOX_Y, framePtr, activityAnimation.frameW, activityAnimation.frameH, SSD1306_WHITE);
}

// March speed: ms it takes to advance 1px. Based on millis() so the
// movement is smooth and constant, regardless of the redraw interval.
const uint32_t ACTIVITY_DOTS_MS_PER_PX = 25;

void drawPlusShape(Adafruit_SSD1306 &display, int16_t left, int16_t top, uint8_t size)
{
    // Star as "+": horizontal and vertical arms crossed at the center of a size x size area.
    const int16_t cx = left + (size - 1) / 2;
    const int16_t cy = top + (size - 1) / 2;
    display.drawFastHLine(left, cy, size, SSD1306_WHITE);
    display.drawFastVLine(cx, top, size, SSD1306_WHITE);
}

void drawSessionMarch(Adafruit_SSD1306 &display, uint8_t sessions, bool plus)
{
    // 2 shapes (3x3) per session in parallel, all visible, marching to the right and
    // reappearing on the left. INTEGER spacing that divides perfectly (spacing*count <= w),
    // centered, leaving margin on the sides: this way all gaps are identical (no gap from
    // rounding). A session in busy guarantees >=1 (2 shapes).
    clearActivityBox(display);
    const uint8_t w = activityAnimation.frameW;
    const uint8_t h = activityAnimation.frameH;
    if (w < 3 || h < 1)
    {
        return;
    }

    uint8_t shape = 3;
    if (shape > h)
    {
        shape = h;
    }
    uint16_t count = static_cast<uint16_t>(sessions < 1 ? 1 : sessions) * 2;
    const uint16_t maxShapes = w / (shape + 1); // shape + 1px minimum separation
    if (maxShapes >= 1 && count > maxShapes)
    {
        count = maxShapes;
    }
    if (count < 1)
    {
        count = 1;
    }

    const uint16_t spacing = w / count;     // exact integer spacing
    const uint16_t period = spacing * count; // width actually used (<= w)
    // Stuck to the RIGHT edge of the box; the leftover (w - period) stays as left margin.
    const int16_t startX = activityBoxX() + (w - period);
    const int16_t topY = ACTIVITY_BOX_Y + ((h > shape) ? (h - shape) / 2 : 0);
    const uint16_t offset = static_cast<uint16_t>((millis() / ACTIVITY_DOTS_MS_PER_PX) % period);
    for (uint16_t i = 0; i < count; i++)
    {
        const int16_t x = startX + (i * spacing + offset) % period;
        if (plus)
        {
            drawPlusShape(display, x, topY, shape);
        }
        else
        {
            display.fillRect(x, topY, shape, shape, SSD1306_WHITE);
        }
    }
}

void drawSessionBars(Adafruit_SSD1306 &display, uint8_t sessions)
{
    // 2 bars per session (equalizer), fixed positions with exact integer spacing and
    // heights that change to give a sense of activity.
    clearActivityBox(display);
    const uint8_t w = activityAnimation.frameW;
    const uint8_t h = activityAnimation.frameH;
    if (w < 2 || h < 1)
    {
        return;
    }

    const uint8_t barW = 2;
    uint16_t bars = static_cast<uint16_t>(sessions < 1 ? 1 : sessions) * 2;
    const uint16_t maxBars = w / (barW + 1);
    if (maxBars >= 1 && bars > maxBars)
    {
        bars = maxBars;
    }
    if (bars < 1)
    {
        bars = 1;
    }

    const uint16_t spacing = w / bars;
    const uint16_t period = spacing * bars;
    // Stuck to the RIGHT edge of the box; the leftover stays as left margin.
    const int16_t startX = activityBoxX() + (w - period);
    const uint8_t tall = h;
    const uint8_t mid = ((h * 2) / 3 >= 1) ? (h * 2) / 3 : 1;
    const uint8_t low = (h / 3 >= 1) ? (h / 3) : 1;
    const uint8_t pattern[4] = {mid, tall, low, mid};
    const uint8_t step = static_cast<uint8_t>(millis() / 120);
    for (uint16_t i = 0; i < bars; i++)
    {
        const int16_t x = startX + i * spacing;
        const uint8_t barH = pattern[(step + i) % 4];
        display.fillRect(x, ACTIVITY_BOX_Y + (h - barH), barW, barH, SSD1306_WHITE);
    }
}

void drawSessionActivity(Adafruit_SSD1306 &display, uint8_t sessions, uint8_t animIndex)
{
    // Discrete styles: encode the count (2 shapes per session). The rest: pure animation.
    if (activityStyle == "stars-right")
    {
        drawSessionMarch(display, sessions, true);
    }
    else if (activityStyle == "bars")
    {
        drawSessionBars(display, sessions);
    }
    else if (activityStyle == "dots" || activityStyle == "dots-right")
    {
        drawSessionMarch(display, sessions, false);
    }
    else if (activityAnimation.valid)
    {
        // spinner / pulse / ball / wave / worm: the service packet plays without a count.
        drawActivityFrame(display, animIndex % activityAnimation.frameCount);
    }
    else
    {
        clearActivityBox(display);
    }
}

void serviceDisplayActivity(Adafruit_SSD1306 &display, const bool ready, const String &activity, const uint8_t sessions, DisplayActivityRuntime &runtime, const uint32_t nowMs)
{
    if (!ready)
    {
        return;
    }

    const bool waiting = (activity == "waiting");
    const bool busy = (activity == "busy");

    // Waiting for the user: the screen blinks inverted to grab attention.
    if (waiting && activityAnimation.invertOnWaiting)
    {
        if (nowMs - runtime.lastInvertToggleMs >= activityAnimation.blinkMs)
        {
            runtime.invertOn = !runtime.invertOn;
            display.invertDisplay(runtime.invertOn);
            runtime.lastInvertToggleMs = nowMs;
        }
        return;
    }

    if (runtime.invertOn)
    {
        runtime.invertOn = false;
        display.invertDisplay(false);
    }

    // Busy: the style decides. Discrete styles draw 2 shapes per session in
    // parallel (count); the rest plays the packaged animation from the service.
    if (busy && activityAnimation.valid && framesReceived)
    {
        const bool firstStep = !runtime.animActive;
        if (firstStep || (nowMs - runtime.lastAnimStepMs >= activityAnimation.intervalMs))
        {
            if (firstStep)
            {
                runtime.animActive = true;
                runtime.animIndex = 0;
            }
            else
            {
                runtime.animIndex++;
            }
            runtime.lastAnimStepMs = nowMs;
            drawSessionActivity(display, sessions, runtime.animIndex);
            display.display();
        }
        return;
    }

    // No activity: if a frame was still painted, clear the corner just once.
    if (runtime.animActive)
    {
        runtime.animActive = false;
        clearActivityBox(display);
        display.display();
    }
}

void updateActivityEffects()
{
    // Only over real frames (not on boot logs, emergency or offline).
    if (liveLogDisplayEnabled || !framesReceived || emergencyWifiActive)
    {
        return;
    }

    const uint32_t nowMs = millis();
    // Swapped displays: the "claude" OLED shows Codex content, activity AND session count.
    serviceDisplayActivity(claudeOled, settings.claudeDisplay.enabled && claudeDisplayReady, codexContentActivity, codexBusySessions, claudeRuntime, nowMs);
    serviceDisplayActivity(codexOled, settings.codexDisplay.enabled && codexDisplayReady, claudeContentActivity, claudeBusySessions, codexRuntime, nowMs);
}

bool connectToWifi(const uint32_t timeoutMs)
{
    liveLogDisplayEnabled = true;
    emergencyWifiActive = false;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(ESP_HOSTNAME);
    WiFi.begin(settings.wifi.ssid.c_str(), settings.wifi.password.c_str());

    logPrint(String("Connecting to ") + settings.wifi.ssid);
    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < timeoutMs)
    {
        logPrint(String("."));
        delay(WIFI_RETRY_DELAY_MS);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.disconnect(false, false);
        logPrintlnEmpty();
        logPrint(String("WiFi failed. SSID: "));
        logPrintln(settings.wifi.ssid);
        return false;
    }

    logPrintlnEmpty();
    logPrint(String("WiFi connected. IP: "));
    logPrintln(WiFi.localIP().toString());
    logPrint(String("Local hostname: "));
    logPrintln(String(LOCAL_DOMAIN));
    return true;
}

void startEmergencyWifi()
{
    emergencyWifiActive = true;
    liveLogDisplayEnabled = false;
    WiFi.mode(WIFI_AP);
    const bool accessPointStarted = WiFi.softAP(EMERGENCY_AP_SSID, EMERGENCY_AP_PASSWORD);
    monitorState.serviceOnline = false;
    monitorState.serviceError = "Emergency WiFi mode";

    if (!accessPointStarted)
    {
        liveLogDisplayEnabled = true;
        logPrintln(String("Could not create the emergency WiFi."));
        return;
    }

    logPrint(String("Emergency WiFi active SSID="));
    logPrint(String(EMERGENCY_AP_SSID));
    logPrint(String(" IP="));
    logPrintln(WiFi.softAPIP().toString());
    drawEmergencyWifiDisplays();
}

void ensureWifiConnected()
{
    if (emergencyWifiActive)
    {
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    logPrintln(String("WiFi disconnected. Retrying connection."));
    if (!connectToWifi(WIFI_CONNECT_TIMEOUT_MS))
    {
        startEmergencyWifi();
    }
}

void handleHomePage()
{
    server.send(200, "text/html", getPageHtml());
}

void handleStatus()
{
    server.send(200, "application/json", getStatusJson());
}

void handleLogs()
{
    server.send(200, "application/json", getLogsJson());
}

void handleGetConfig()
{
    server.send(200, "application/json", getSettingsJson());
}

void applyDisplaySettings(const JsonVariantConst source, DisplaySettings &displaySettings)
{
    displaySettings.enabled = source["enabled"] | displaySettings.enabled;
    displaySettings.sdaPin = source["sda"] | displaySettings.sdaPin;
    displaySettings.sclPin = source["scl"] | displaySettings.sclPin;
    displaySettings.address = source["address"] | displaySettings.address;
    displaySettings.contrast = source["contrast"] | displaySettings.contrast;
}

void handleSaveConfig()
{
    const String body = server.arg("plain");
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, body);

    if (error)
    {
        server.send(400, "application/json", String("{\"status\":\"error\",\"message\":") + stringToJson(error.c_str()) + String("}"));
        return;
    }

    bool wifiChanged = false;
    const JsonVariantConst wifiSource = document["wifi"];
    if (!wifiSource.isNull())
    {
        String nextSsid = wifiSource["ssid"] | settings.wifi.ssid;
        nextSsid.trim();

        if (nextSsid.length() == 0 || nextSsid.length() > 32)
        {
            server.send(400, "application/json", String("{\"status\":\"error\",\"message\":\"The WiFi SSID must be between 1 and 32 characters.\"}"));
            return;
        }

        if (nextSsid != settings.wifi.ssid)
        {
            settings.wifi.ssid = nextSsid;
            wifiChanged = true;
        }

        const JsonVariantConst passwordSource = wifiSource["password"];
        if (!passwordSource.isNull())
        {
            const String nextPassword = passwordSource.as<String>();
            if (nextPassword.length() > 0)
            {
                if (nextPassword.length() < 8 || nextPassword.length() > 63)
                {
                    server.send(400, "application/json", String("{\"status\":\"error\",\"message\":\"The WiFi password must be between 8 and 63 characters.\"}"));
                    return;
                }

                if (nextPassword != settings.wifi.password)
                {
                    settings.wifi.password = nextPassword;
                    wifiChanged = true;
                }
            }
        }
    }

    settings.serviceUrl = document["service_url"] | settings.serviceUrl;
    settings.pollIntervalMs = document["poll_interval_ms"] | settings.pollIntervalMs;
    if (settings.pollIntervalMs < 1000)
    {
        settings.pollIntervalMs = 1000;
    }
    applyDisplaySettings(document["claude_display"], settings.claudeDisplay);
    applyDisplaySettings(document["codex_display"], settings.codexDisplay);
    saveSettings();
    initializeDisplays();
    if (emergencyWifiActive)
    {
        drawEmergencyWifiDisplays();
    }
    if (wifiChanged)
    {
        configRestartPending = true;
        configRestartAtMs = millis() + RESTART_DELAY_MS;
    }
    server.send(200, "application/json", getSettingsJson());
}

void sendOtaUploadResult()
{
    server.sendHeader("Connection", "close");

    if (webOtaError || Update.hasError())
    {
        server.send(500, "application/json", String("{\"status\":\"error\",\"message\":") + stringToJson(webOtaStatus) + String("}"));
        return;
    }

    webOtaRestartPending = true;
    webOtaRestartAtMs = millis() + RESTART_DELAY_MS;
    server.send(200, "application/json", String("{\"status\":\"ok\",\"message\":") + stringToJson(webOtaStatus) + String("}"));
}

void handleOtaUpload()
{
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        webOtaBytes = 0;
        webOtaError = false;
        webOtaRestartPending = false;
        webOtaStatus = String("Receiving ") + upload.filename;

        if (!upload.filename.endsWith(".bin"))
        {
            webOtaError = true;
            webOtaStatus = "The file must end in .bin.";
            return;
        }

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
        {
            webOtaError = true;
            webOtaStatus = String("Could not start the update: ") + Update.errorString();
            return;
        }

        logPrint(String("Web OTA started: "));
        logPrintln(upload.filename);
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (webOtaError)
        {
            return;
        }

        const size_t writtenBytes = Update.write(upload.buf, upload.currentSize);
        webOtaBytes = upload.totalSize;
        if (writtenBytes != upload.currentSize)
        {
            webOtaError = true;
            webOtaStatus = String("Error writing firmware: ") + Update.errorString();
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END)
    {
        if (webOtaError)
        {
            Update.abort();
            return;
        }

        if (!Update.end(true))
        {
            webOtaError = true;
            webOtaStatus = String("Could not finalize the update: ") + Update.errorString();
            return;
        }

        webOtaBytes = upload.totalSize;
        webOtaStatus = String("Firmware received: ") + String(webOtaBytes) + String(" bytes.");
    }

    if (upload.status == UPLOAD_FILE_ABORTED)
    {
        Update.abort();
        webOtaError = true;
        webOtaStatus = "Upload canceled before finishing.";
    }
}

void startWebServer()
{
    server.on("/", HTTP_GET, handleHomePage);
    server.on("/home", HTTP_GET, handleHomePage);
    server.on("/ota", HTTP_GET, handleHomePage);
    server.on("/config", HTTP_GET, handleHomePage);
    server.on("/logs", HTTP_GET, handleHomePage);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/logs", HTTP_GET, handleLogs);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handleSaveConfig);
    server.on("/ota/upload", HTTP_POST, sendOtaUploadResult, handleOtaUpload);
    server.begin();

    logPrintln(String("Web server ready on port 80"));
}

void startMdnsServices()
{
    if (!MDNS.begin(ESP_HOSTNAME))
    {
        logPrintln(String("Could not start mDNS."));
        return;
    }

    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.enableArduino(OTA_PORT, String(OTA_PASSWORD).length() > 0);
    logPrint(String("mDNS ready: "));
    logPrintln(String(LOCAL_DOMAIN));
}

void startOta()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setMdnsEnabled(false);

    ArduinoOTA.onStart([]() {
        logPrintln(String("PlatformIO OTA started"));
    });

    ArduinoOTA.onEnd([]() {
        logPrintlnEmpty();
        logPrintln(String("PlatformIO OTA finished"));
    });

    ArduinoOTA.onError([](const ota_error_t error) {
        logPrint(String("OTA error: "));
        logPrintln(String(static_cast<uint8_t>(error)));
    });

    ArduinoOTA.begin();
    logPrint(String("OTA ready. Hostname: "));
    logPrintln(String(OTA_HOSTNAME));
}

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(1000);
    logPrintln(String("ESP32 Monitor boot"));

    monitorState.serviceOnline = false;
    monitorState.serviceError = "No initial query";
    monitorState.lastPollMs = 0;
    monitorState.lastSuccessMs = 0;
    monitorState.frameUpdatedAtMs = 0;

    loadSettings();
    logPrint(String("Frames service: "));
    logPrintln(settings.serviceUrl);
    initializeDisplays();
    logPrint(String("OLED Claude: "));
    logPrintln(claudeDisplayReady ? String("OK") : String("not available"));
    logPrint(String("OLED Codex: "));
    logPrintln(codexDisplayReady ? String("OK") : String("not available"));
    const bool wifiConnected = connectToWifi(WIFI_CONNECT_TIMEOUT_MS);
    if (!wifiConnected)
    {
        startEmergencyWifi();
    }
    startWebServer();
    if (wifiConnected)
    {
        startMdnsServices();
        startOta();
        // Triggers the first frame fetch immediately in the loop (without blocking setup):
        // leaving lastPoll one interval back makes serviceFramesFetch start right away.
        monitorState.lastPollMs = millis() - settings.pollIntervalMs;
        fetchActivityAnimation();
    }
}

void loop()
{
    server.handleClient();

    if (configRestartPending && millis() >= configRestartAtMs)
    {
        logPrintln(String("Restarting due to WiFi change"));
        ESP.restart();
    }

    if (emergencyWifiActive)
    {
        return;
    }

    ensureWifiConnected();
    if (emergencyWifiActive)
    {
        return;
    }

    ArduinoOTA.handle();

    // Non-blocking frame fetch: starts every pollInterval and is read in chunks without
    // freezing the loop (previously a synchronous GET that paused the render ~1s).
    serviceFramesFetch();

    // If the service announces another animation version, it is re-downloaded and stored.
    if (pendingAnimEtag.length() > 0 && pendingAnimEtag != activityAnimationEtag)
    {
        fetchActivityAnimation();
    }

    // Non-blocking long-poll: updates claude/codexContentActivity almost instantly.
    serviceActivityLongPoll();

    // Anti burn-in screensaver: if the service has been down for a while, instead of
    // leaving the fixed image, the animation chosen in config is rendered full screen.
    const bool serviceDownLongEnough = !monitorState.serviceOnline &&
                                       (millis() - monitorState.lastSuccessMs) > SAVER_GRACE_MS;
    if (serviceDownLongEnough)
    {
        if (!saverRunning)
        {
            saverRunning = true;
            liveLogDisplayEnabled = false;
            startScreenSaver();
        }
        if (millis() - lastSaverFrameMs >= SAVER_FRAME_MS)
        {
            lastSaverFrameMs = millis();
            renderScreenSaver();
        }
    }
    else
    {
        saverRunning = false;
        // Animates the corner (busy) or inverts the screen (waiting for a response).
        updateActivityEffects();
    }

    if (webOtaRestartPending && millis() >= webOtaRestartAtMs)
    {
        logPrintln(String("Restarting due to web OTA"));
        ESP.restart();
    }
}
