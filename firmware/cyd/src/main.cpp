#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_system.h>

// Las credenciales por defecto viven en secrets.h (ignorado por git). Si no existe,
// se compila con valores vacios y el CYD arranca en el AP de emergencia.
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
#ifndef SECRET_SERVICE_URL
#define SECRET_SERVICE_URL ""
#endif
#ifndef SECRET_WEB_PASSWORD
#define SECRET_WEB_PASSWORD ""
#endif

// ---------------------------------------------------------------------------
// Constantes de red, identidad y hardware del CYD.
// ---------------------------------------------------------------------------

const uint16_t HTTP_PORT = 80;
const char WEB_USER[] = "admin"; // usuario de la auth basica de la web (usuario admin)
const char DEFAULT_WEB_PASSWORD[] = SECRET_WEB_PASSWORD; // clave web por defecto; vacia = web abierta
const uint32_t MIN_ONLINE_CYCLE_MS = 500; // piso de cadencia del sondeo online (anti busy-loop)
const char ESP_HOSTNAME[] = "cyd";
const char LOCAL_DOMAIN[] = "cyd.local";
const char OTA_HOSTNAME[] = "cyd";
const char OTA_PASSWORD[] = SECRET_OTA_PASSWORD;
const char DEFAULT_WIFI_SSID[] = SECRET_WIFI_SSID;
const char DEFAULT_WIFI_PASSWORD[] = SECRET_WIFI_PASSWORD;
const char DEFAULT_SERVICE_URL[] = SECRET_SERVICE_URL;
const char EMERGENCY_AP_SSID[] = "CYD-Emergencia";
const char EMERGENCY_AP_PASSWORD[] = "configcyd0";
const char EMERGENCY_AP_URL[] = "http://192.168.4.1";

// El LED RGB del CYD (GPIO 4/16/17) es activo en bajo: se fuerza a HIGH para apagarlo.
const uint8_t RGB_LED_PINS[] = {4, 16, 17};
// PENIRQ del tactil XPT2046: se pone en LOW al tocar la pantalla (sin SPI ni calibracion).
const uint8_t TOUCH_IRQ_PIN = 36;
// Ranura microSD del CYD (bus HSPI propio; el TFT usa VSPI, no chocan).
const uint8_t SD_CS_PIN = 5;
const uint8_t SD_SCLK_PIN = 18;
const uint8_t SD_MISO_PIN = 19;
const uint8_t SD_MOSI_PIN = 23;
const char LOG_FILE_PATH[] = "/cyd-log.txt";
const uint32_t LOG_FILE_MAX_BYTES = 5242880; // 5MB antes de rotar
const uint8_t LOG_FILE_KEEP = 5;             // cantidad de archivos rotados que se conservan
const char BACKUP_DIR[] = "/backups";
const char COREDUMP_DIR[] = "/coredumps"; // volcados de crash guardados en la SD
const uint8_t COREDUMP_KEEP = 3;          // LIMITE: solo se conservan los ultimos N (rota por slot)
// Version del esquema de config: se guarda en cada backup para poder migrar en el futuro.
// Subirla cuando cambie la ESTRUCTURA de config (campos nuevos/renombrados).
const uint16_t CONFIG_VERSION = 2; // v2: incluye paleta completa + tendencia + tamano de cuadros
// Retroiluminacion por PWM (LEDC) para poder atenuar (anti burn-in).
const uint8_t BL_LEDC_CHANNEL = 0;
const uint32_t BL_LEDC_FREQ = 5000;
const uint8_t BL_LEDC_BITS = 8;

// Dimensiones de la pantalla en horizontal (rotacion 1/3).
const int16_t SCREEN_W = 320;
const int16_t SCREEN_H = 240;
const int16_t PANEL_H = SCREEN_H / 2; // Un panel por herramienta (Claude arriba, Codex abajo).

// Tiempos del sondeo.
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
const uint32_t SNAPSHOT_HTTP_TIMEOUT_MS = 4000;

// ---------------------------------------------------------------------------
// Configuracion persistente (NVS) y estado de ejecucion.
// ---------------------------------------------------------------------------

struct AppConfig
{
    String wifiSsid;
    String wifiPassword;
    String serviceUrl;
    uint32_t pollIntervalMs;
    uint8_t rotation;         // 1 o 3 (horizontal). Permite girar 180 grados.
    uint8_t brightnessPct;    // Brillo activo (0-100).
    uint8_t dimBrightnessPct; // Brillo atenuado por inactividad (0-100).
    uint32_t dimAfterSec;     // Segundos de inactividad total antes de atenuar (0 = desactivado).
    bool invertDisplay;       // Inversion de color del ILI9341 (el CYD suele necesitarla).
    uint8_t animSpeed;        // Velocidad del marquee busy (px por tick, 1-15).
    int16_t animBoxX;         // Borde izquierdo del area de animacion (px, 110-280).
    String waitTitle;         // Titulo grande del panel de pregunta (waiting).
    String waitMsg;           // Mensaje del panel de pregunta; se le antepone el nombre.
    String webPassword;       // Clave de la auth web (usuario admin). Vacia = web abierta.
    uint8_t screensaver;      // Screensaver offline: 0 off, 1 matrix, 2 dvd, 3 stars, 4 snake.
    String claudeColor;       // Color de acento de Claude, hex "RRGGBB".
    String codexColor;        // Color de acento de Codex, hex "RRGGBB".
    String textColor;         // Color del texto principal (digitos del reset), hex "RRGGBB".
    String mutedColor;        // Color del texto secundario/gris (5H, WK, unidades H/D/M), hex "RRGGBB".
    uint16_t trendWindowMin;  // Ventana para la tendencia del superavit, en minutos.
    uint8_t trendMarginPct;   // Margen muerto de la tendencia (puntos %); dentro de +-margen = estable.
    uint8_t animSquarePx;     // Tamano (px) de los cuadrados de la animacion busy, 4-14.
    // Paleta completa configurable (todos hex "RRGGBB"). Los acentos y textos ya existen arriba.
    String bgColor;           // Fondo de la pantalla.
    String dividerColor;      // Linea divisora de cada panel.
    String barBgColor;        // Fondo de las barras de uso.
    String goodColor;         // Positivo (tendencia arriba, estados OK).
    String warnColor;         // Advertencia.
    String badColor;          // Negativo (deficit, tendencia abajo, errores).
    String disabledColor;     // Texto/marcas de estado deshabilitado / idle.
    uint8_t alertThresholdPct; // Si el % restante <= este umbral, el numero y la barra van en rojo. 0 = off.
    String claudeLabel;       // Alias local para Claude (vacio = usar el nombre del servicio).
    String codexLabel;        // Alias local para Codex (vacio = usar el nombre del servicio).
    uint8_t animStyle;        // Estilo de la animacion busy: 0 marquee, 1 pulso, 2 barra.
    uint16_t alertBlinkMs;    // Periodo del parpadeo de la alerta de pregunta (ms). 0 = panel fijo.
    uint8_t animHz;           // Cadencia (fps/Hz) de la animacion de actividad: 10-60.
};

// Datos de una ventana de uso (5h o semanal) tal como los entrega el snapshot.
struct WindowData
{
    float remaining = 0.0f;   // remaining_percent
    float expected = 0.0f;    // expected_remaining_percent (regresion lineal)
    String pace;              // under / on_track / over / unknown
    long resetSeconds = 0;    // reset_in_seconds
};

// Estado de una herramienta (Claude o Codex).
struct ToolData
{
    String label;
    bool enabled = true;
    String activity = "idle"; // idle / busy / waiting
    String statusText;
    WindowData current;
    WindowData weekly;
};

struct MonitorState
{
    bool online = false;
    String lastError;
    ToolData claude;
    ToolData codex;
    uint32_t lastPollMs = 0;
    uint32_t lastActivityChangeMs = 0;
};

// Paleta (se llena en setup con color565).
struct Palette
{
    uint16_t bg;
    uint16_t panelDivider;
    uint16_t textPrimary;
    uint16_t textMuted;
    uint16_t barBg;
    uint16_t good;
    uint16_t ok;
    uint16_t warn;
    uint16_t bad;
    uint16_t muted;
    uint16_t claude;
    uint16_t codex;
    uint16_t tick;
};

WebServer server(HTTP_PORT);
Preferences preferences;
TFT_eSPI tft = TFT_eSPI();

AppConfig config;
MonitorState monitorState;
Palette pal;

bool emergencyWifiActive = false;
bool needsFullRedraw = true;
uint32_t lastAnimTickMs = 0;
uint8_t appliedBrightnessPct = 255; // Fuerza aplicar el brillo la primera vez.
uint8_t brightnessTargetPct = 100;  // Objetivo hacia el que converge appliedBrightnessPct (rampa suave).

// Sprite fuera de pantalla del tamano de un panel: los paneles se dibujan aqui y se
// vuelcan de una sola vez (pushSprite), eliminando el flicker del redibujado directo.
TFT_eSprite panelSprite = TFT_eSprite(&tft);
// Franja pequena para animar la actividad "busy" volcando SOLO su area (no los 320x120 del
// panel): reduce muchisimo el SPI por frame y con ello el tearing (lineas diagonales).
TFT_eSprite stripSprite = TFT_eSprite(&tft);
bool stripReady = false;
// Aviso de pregunta: el panel invertido se vuelca UNA vez (base estable) y solo parpadea un
// banner "INPUT" en una franja chica (titleSprite) -> el flash no vuelca los 320x120 = sin tearing.
// Sprite overlay COMPARTIDO del aviso de pregunta (una sola reserva, en setup): en el se dibujan
// y vuelcan por separado, en su posicion, tanto el icono+nombre (arriba) como "INPUT"+subtitulo
// (centro). Volcados chicos -> sin tearing; una sola reserva -> sin fragmentar el heap.
const int16_t OVL_W = 300, OVL_H = 44; // ancho para que 34 chars entren a escala 2 fija (sin achicar)
TFT_eSprite ovlSprite = TFT_eSprite(&tft);
bool ovlReady = false;
bool waitBase[2] = {false, false}; // base (fondo acento) ya dibujada (0=claude, 1=codex)

// Reserva/libera un sprite bajo demanda (para tener el maximo heap libre en idle online).
// bpp: profundidad de color. Los sprites de 2 colores (alerta, franja busy) usan 4bpp con paleta
// (1/4 de RAM que 16bpp, colores EXACTOS via createPalette); el resto usa 16bpp.
bool ensureSprite(TFT_eSprite &spr, bool &ready, int16_t w, int16_t h, uint8_t bpp)
{
    if (!ready)
    {
        spr.setColorDepth(bpp);
        ready = (spr.createSprite(w, h) != nullptr);
    }
    return ready;
}
void freeSprite(TFT_eSprite &spr, bool &ready)
{
    if (ready)
    {
        spr.deleteSprite();
        ready = false;
    }
}

// Almacenamiento: LittleFS (FS interno) y tarjeta SD (bus HSPI propio). Los logs van a
// la SD si esta presente; si no, se quedan solo en RAM (evita desgastar la flash interna).
SPIClass sdSpi(HSPI);
bool sdReady = false;
bool littleFsReady = false;

// Parpadeo del panel invertido cuando hay una pregunta (waiting).
bool blinkOn = false;
uint32_t lastBlinkMs = 0;

// Overlay de diagnostico al tocar la pantalla (toggle con cada toque).
bool infoOverlayActive = false;
uint32_t lastOverlayDrawMs = 0;

// Metricas de red para el overlay (las actualiza la tarea de sondeo; lectura de uint32
// es atomica en ESP32, sirve para mostrar).
volatile uint32_t statLastPollMs = 0;
volatile uint32_t statLastSuccessMs = 0;
volatile uint32_t statPollCount = 0;
volatile uint32_t statFailCount = 0;
volatile uint32_t pollHeartbeat = 0;              // incrementa al tope de cada vuelta de pollTask
const uint32_t POLL_STALL_MS = 120000;            // sin latido tanto tiempo = pollTask colgado -> reinicio
// Ritmo del loop de render (core1) como proxy de carga de CPU: vueltas por segundo.
uint32_t loopCount = 0, loopHz = 0, lastHzMs = 0, lastHzCount = 0;

// Sesiones (agentes) trabajando en paralelo por herramienta, de las cabeceras
// X-Sessions-* del servicio. Definen cuantos puntos muestra la animacion "dots-right".
uint8_t claudeSessions = 1;
uint8_t codexSessions = 1;
String framesEtag = "";
// Desplazamiento del marquee de cuadrados de la animacion busy (contador libre; se mueve
// a la derecha). Es uint32 para que solo haya discontinuidad al desbordar (nunca en la practica).
uint32_t dotScroll = 0;

// Tendencia del superavit por bloque (0 Claude 5H, 1 Claude WK, 2 Codex 5H, 3 Codex WK).
// Se guarda un historico corto de muestras y se compara el superavit actual contra el de
// hace config.trendWindowMin minutos para saber si subio (+1), bajo (-1) o quedo dentro del
// margen (0). Se dibuja una flecha/punto a la derecha del delta de cada ventana.
const uint8_t TREND_BLOCKS = 4;
const uint8_t TREND_SAMPLES = 48;
const uint32_t RECENT_TREND_MS = 360000UL; // la flecha reacciona al uso de los ultimos ~6 min (no promedia la ventana entera)
struct TrendSample
{
    uint32_t ms;
    float surplus;
};
struct TrendHist
{
    TrendSample s[TREND_SAMPLES];
    uint8_t count;
    uint8_t head;
};
TrendHist trendHist[TREND_BLOCKS] = {};
int8_t trendDir[TREND_BLOCKS] = {0, 0, 0, 0};

bool webOtaError = false;
bool webOtaRestartPending = false;
uint32_t webOtaRestartAtMs = 0;
String webOtaStatus = "Listo para subir firmware.";

// Sondeo HTTP en una tarea del segundo nucleo (core 0): asi el loop de render/animacion
// (core 1) nunca se bloquea esperando a la red y la animacion no se traba. La tarea
// publica el estado en incomingState bajo mutex; el loop lo copia a monitorState y dibuja
// (todo el acceso a la TFT ocurre solo en el loop, nunca en la tarea).
SemaphoreHandle_t stateMutex = nullptr;
// Protege las lecturas/escrituras de los String de config compartidos entre core0 (pollTask)
// y core1 (handlers web). pollTask copia serviceUrl/pollInterval bajo este mutex al inicio de
// cada ciclo; los handlers lo toman al mutar config (evita use-after-free del buffer del String).
SemaphoreHandle_t configMutex = nullptr;
TaskHandle_t pollTaskHandle = nullptr;
MonitorState incomingState;
volatile bool incomingReady = false;
bool spriteReady = false;   // el sprite de panel se creo correctamente
bool wifiDownDrawn = false;  // ya se dibujo el aviso de WiFi caido (evita repintar)

// Prototipos de funciones referenciadas antes de su definicion.
String getActiveWifiSsidSafe();
String normHexColor(const String &hex, const String &def);

// ---------------------------------------------------------------------------
// Utilidades.
// ---------------------------------------------------------------------------

String jsonEscape(const String &value)
{
    String out = "";
    out.reserve(value.length() + value.length() / 8 + 8); // evita cientos de reallocaciones (logBuffer ~8KB)
    for (size_t i = 0; i < value.length(); i++)
    {
        const char c = value.charAt(i);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else if (c == '\t')
        {
            out += "\\t";
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// Formatea segundos restantes como duracion legible: XDdYh / HhMMm / Mm.
String formatDuration(long seconds)
{
    if (seconds < 0)
    {
        seconds = 0;
    }
    const long minutes = seconds / 60;
    const long hours = minutes / 60;
    if (hours >= 24)
    {
        return String(hours / 24) + "D" + String(hours % 24) + "H";
    }
    if (hours >= 1)
    {
        char buffer[12];
        snprintf(buffer, sizeof(buffer), "%ldH%02ldM", hours, minutes % 60);
        return String(buffer);
    }
    return String(minutes) + "M";
}

// Texto del delta remaining - expected: +N% (margen) o -N% (pasado de ritmo).
String formatDelta(const WindowData &window)
{
    const int delta = (int)lround(window.remaining - window.expected);
    if (delta >= 0)
    {
        return String("+") + String(delta) + "%";
    }
    return String(delta) + "%";
}

float clamp01(float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }
    if (value > 100.0f)
    {
        return 100.0f;
    }
    return value;
}

// ---------------------------------------------------------------------------
// Configuracion persistente.
// ---------------------------------------------------------------------------

// Normaliza y acota TODOS los campos de config. La usan tanto loadConfig (al arranque)
// como handlePostConfig (al guardar por web), asi que un POST no puede persistir valores
// peligrosos (brillo=0 -> pantalla negra, animBoxX invalido -> modulo por cero, etc.).
void sanitizeConfig()
{
    if (config.rotation != 1 && config.rotation != 3)
    {
        config.rotation = 1;
    }
    if (config.pollIntervalMs < 1000)
    {
        config.pollIntervalMs = 1000;
    }
    if (config.pollIntervalMs > 60000)
    {
        config.pollIntervalMs = 60000;
    }
    if (config.brightnessPct < 5)
    {
        config.brightnessPct = 5; // nunca 0: evita dejar la pantalla apagada permanente
    }
    if (config.brightnessPct > 100)
    {
        config.brightnessPct = 100;
    }
    if (config.dimBrightnessPct < 5)
    {
        config.dimBrightnessPct = 5; // piso 5: evita apagar la pantalla (0%) estando online
    }
    if (config.dimBrightnessPct > 100)
    {
        config.dimBrightnessPct = 100;
    }
    if (config.dimAfterSec > 86400)
    {
        config.dimAfterSec = 86400; // tope 24h: evita desbordar dimAfterSec*1000
    }
    if (config.animSpeed < 1 || config.animSpeed > 15)
    {
        config.animSpeed = 6;
    }
    if (config.animBoxX < 110 || config.animBoxX > 280)
    {
        config.animBoxX = 112;
    }
    if (config.waitTitle.length() > 10)
    {
        config.waitTitle = config.waitTitle.substring(0, 10);
    }
    if (config.waitMsg.length() > 34)
    {
        config.waitMsg = config.waitMsg.substring(0, 34);
    }
    if (config.screensaver > 4)
    {
        config.screensaver = 1;
    }
    config.claudeColor = normHexColor(config.claudeColor, "D97757");
    config.codexColor = normHexColor(config.codexColor, "28B48C");
    config.textColor = normHexColor(config.textColor, "EBEDF0");
    config.mutedColor = normHexColor(config.mutedColor, "8C929C");
    if (config.trendWindowMin < 1 || config.trendWindowMin > 1440)
    {
        config.trendWindowMin = 60;
    }
    if (config.trendMarginPct > 50)
    {
        config.trendMarginPct = 2;
    }
    if (config.animSquarePx < 4 || config.animSquarePx > 14)
    {
        config.animSquarePx = 8;
    }
    config.bgColor = normHexColor(config.bgColor, "000000");
    config.dividerColor = normHexColor(config.dividerColor, "30343C");
    config.barBgColor = normHexColor(config.barBgColor, "262930");
    config.goodColor = normHexColor(config.goodColor, "46C878");
    config.warnColor = normHexColor(config.warnColor, "E8AA2D");
    config.badColor = normHexColor(config.badColor, "E05046");
    config.disabledColor = normHexColor(config.disabledColor, "606670");
    if (config.alertThresholdPct > 100)
    {
        config.alertThresholdPct = 0;
    }
    if (config.claudeLabel.length() > 12)
    {
        config.claudeLabel = config.claudeLabel.substring(0, 12);
    }
    if (config.codexLabel.length() > 12)
    {
        config.codexLabel = config.codexLabel.substring(0, 12);
    }
    if (config.animStyle > 2)
    {
        config.animStyle = 0;
    }
    if (config.animHz < 10 || config.animHz > 60)
    {
        config.animHz = 50; // limites cuerdos: <10 se ve a saltos, >60 solo quema CPU sin ganancia
    }
    if (config.alertBlinkMs > 5000)
    {
        config.alertBlinkMs = 5000;
    }
}

void loadConfig()
{
    preferences.begin("cydmon", true);
    config.wifiSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
    config.wifiPassword = preferences.getString("pass", DEFAULT_WIFI_PASSWORD);
    config.serviceUrl = preferences.getString("service", DEFAULT_SERVICE_URL);
    config.pollIntervalMs = preferences.getUInt("poll", 3000);
    config.rotation = preferences.getUChar("rot", 1);
    config.brightnessPct = preferences.getUChar("bright", 100);
    config.dimBrightnessPct = preferences.getUChar("dimb", 25);
    config.dimAfterSec = preferences.getUInt("dima", 120);
    config.invertDisplay = preferences.getBool("inv", true);
    config.animSpeed = preferences.getUChar("aspd", 6);
    config.animBoxX = preferences.getShort("abx", 112);
    config.waitTitle = preferences.getString("wt", "INPUT");
    config.waitMsg = preferences.getString("wm", "NEEDS YOUR INPUT");
    config.webPassword = preferences.getString("wpass", DEFAULT_WEB_PASSWORD);
    config.screensaver = preferences.getUChar("sav", 1);
    config.claudeColor = preferences.getString("ccol", "D97757");
    config.codexColor = preferences.getString("xcol", "28B48C");
    config.textColor = preferences.getString("tcol", "EBEDF0");
    config.mutedColor = preferences.getString("mcol", "8C929C");
    config.trendWindowMin = preferences.getUShort("twin", 60);
    config.trendMarginPct = preferences.getUChar("tmar", 2);
    config.animSquarePx = preferences.getUChar("asq", 8);
    config.bgColor = preferences.getString("bgc", "000000");
    config.dividerColor = preferences.getString("dvc", "30343C");
    config.barBgColor = preferences.getString("bbc", "262930");
    config.goodColor = preferences.getString("gdc", "46C878");
    config.warnColor = preferences.getString("wnc", "E8AA2D");
    config.badColor = preferences.getString("bdc", "E05046");
    config.disabledColor = preferences.getString("dsc", "606670");
    config.alertThresholdPct = preferences.getUChar("alrt", 0);
    config.claudeLabel = preferences.getString("clbl", "");
    config.codexLabel = preferences.getString("xlbl", "");
    config.animStyle = preferences.getUChar("asty", 0);
    config.animHz = preferences.getUChar("ahz", 50);
    config.alertBlinkMs = preferences.getUShort("ablk", 400);
    preferences.end();
    sanitizeConfig();
}

bool saveConfig()
{
    if (!preferences.begin("cydmon", false))
    {
        return false; // NVS no abrio para escritura: el caller no debe confirmar ni reiniciar
    }
    preferences.putString("ssid", config.wifiSsid);
    preferences.putString("pass", config.wifiPassword);
    preferences.putString("service", config.serviceUrl);
    preferences.putUInt("poll", config.pollIntervalMs);
    preferences.putUChar("rot", config.rotation);
    preferences.putUChar("bright", config.brightnessPct);
    preferences.putUChar("dimb", config.dimBrightnessPct);
    preferences.putUInt("dima", config.dimAfterSec);
    preferences.putBool("inv", config.invertDisplay);
    preferences.putUChar("aspd", config.animSpeed);
    preferences.putShort("abx", config.animBoxX);
    preferences.putString("wt", config.waitTitle);
    preferences.putString("wm", config.waitMsg);
    preferences.putString("wpass", config.webPassword);
    preferences.putUChar("sav", config.screensaver);
    preferences.putString("ccol", config.claudeColor);
    preferences.putString("xcol", config.codexColor);
    preferences.putString("tcol", config.textColor);
    preferences.putString("mcol", config.mutedColor);
    preferences.putUShort("twin", config.trendWindowMin);
    preferences.putUChar("tmar", config.trendMarginPct);
    preferences.putUChar("asq", config.animSquarePx);
    preferences.putString("bgc", config.bgColor);
    preferences.putString("dvc", config.dividerColor);
    preferences.putString("bbc", config.barBgColor);
    preferences.putString("gdc", config.goodColor);
    preferences.putString("wnc", config.warnColor);
    preferences.putString("bdc", config.badColor);
    preferences.putString("dsc", config.disabledColor);
    preferences.putUChar("alrt", config.alertThresholdPct);
    preferences.putString("clbl", config.claudeLabel);
    preferences.putString("xlbl", config.codexLabel);
    preferences.putUChar("asty", config.animStyle);
    preferences.putUChar("ahz", config.animHz);
    preferences.putUShort("ablk", config.alertBlinkMs);
    preferences.end();
    return true;
}

// Valida un hex "RRGGBB" (6 chars); devuelve def si es invalido.
String normHexColor(const String &hex, const String &def)
{
    String h = hex;
    if (h.startsWith("#"))
    {
        h = h.substring(1);
    }
    if (h.length() != 6)
    {
        return def;
    }
    for (uint8_t i = 0; i < 6; i++)
    {
        if (!isxdigit((unsigned char)h.charAt(i))) // cast obligatorio: char con signo + byte>127 = UB en isxdigit
        {
            return def;
        }
    }
    h.toUpperCase();
    return h;
}

// Convierte hex "RRGGBB" a color565.
uint16_t hexTo565(const String &hex)
{
    const long v = strtol(hex.c_str(), nullptr, 16);
    return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Serializa la config actual a JSON (con version) para respaldarla en la SD.
String configToJson()
{
    JsonDocument d;
    d["version"] = CONFIG_VERSION;
    d["ssid"] = config.wifiSsid;
    d["pass"] = config.wifiPassword;
    d["service"] = config.serviceUrl;
    d["poll"] = config.pollIntervalMs;
    d["rot"] = config.rotation;
    d["bright"] = config.brightnessPct;
    d["dimb"] = config.dimBrightnessPct;
    d["dima"] = config.dimAfterSec;
    d["inv"] = config.invertDisplay;
    d["aspd"] = config.animSpeed;
    d["abx"] = config.animBoxX;
    d["wt"] = config.waitTitle;
    d["wm"] = config.waitMsg;
    d["wpass"] = config.webPassword;
    d["sav"] = config.screensaver;
    // Paleta completa + tendencia + tamano de cuadros (faltaban: el backup los perdia).
    d["ccol"] = config.claudeColor;
    d["xcol"] = config.codexColor;
    d["tcol"] = config.textColor;
    d["mcol"] = config.mutedColor;
    d["bgc"] = config.bgColor;
    d["dvc"] = config.dividerColor;
    d["bbc"] = config.barBgColor;
    d["gdc"] = config.goodColor;
    d["wnc"] = config.warnColor;
    d["bdc"] = config.badColor;
    d["dsc"] = config.disabledColor;
    d["twin"] = config.trendWindowMin;
    d["tmar"] = config.trendMarginPct;
    d["asq"] = config.animSquarePx;
    d["alrt"] = config.alertThresholdPct;
    d["clbl"] = config.claudeLabel;
    d["xlbl"] = config.codexLabel;
    d["asty"] = config.animStyle;
    d["ahz"] = config.animHz;
    d["ablk"] = config.alertBlinkMs;
    String out;
    serializeJsonPretty(d, out);
    return out;
}

// Aplica una config desde un JSON de backup. Carga TOLERANTE: los campos ausentes conservan
// el valor actual, asi un backup viejo (menos campos) se restaura sin romper -> compat hacia
// adelante. d["version"] queda disponible para migraciones explicitas futuras.
bool applyConfigFromJson(const String &json)
{
    JsonDocument d;
    if (deserializeJson(d, json))
    {
        return false;
    }
    config.wifiSsid = String((const char *)(d["ssid"] | config.wifiSsid.c_str()));
    config.wifiPassword = String((const char *)(d["pass"] | config.wifiPassword.c_str()));
    config.serviceUrl = String((const char *)(d["service"] | config.serviceUrl.c_str()));
    config.pollIntervalMs = d["poll"] | config.pollIntervalMs;
    config.rotation = d["rot"] | config.rotation;
    config.brightnessPct = d["bright"] | config.brightnessPct;
    config.dimBrightnessPct = d["dimb"] | config.dimBrightnessPct;
    config.dimAfterSec = d["dima"] | config.dimAfterSec;
    config.invertDisplay = d["inv"] | config.invertDisplay;
    config.animSpeed = d["aspd"] | config.animSpeed;
    config.animBoxX = d["abx"] | config.animBoxX;
    config.waitTitle = String((const char *)(d["wt"] | config.waitTitle.c_str()));
    config.waitMsg = String((const char *)(d["wm"] | config.waitMsg.c_str()));
    config.webPassword = String((const char *)(d["wpass"] | config.webPassword.c_str()));
    config.screensaver = d["sav"] | config.screensaver;
    config.claudeColor = String((const char *)(d["ccol"] | config.claudeColor.c_str()));
    config.codexColor = String((const char *)(d["xcol"] | config.codexColor.c_str()));
    config.textColor = String((const char *)(d["tcol"] | config.textColor.c_str()));
    config.mutedColor = String((const char *)(d["mcol"] | config.mutedColor.c_str()));
    config.bgColor = String((const char *)(d["bgc"] | config.bgColor.c_str()));
    config.dividerColor = String((const char *)(d["dvc"] | config.dividerColor.c_str()));
    config.barBgColor = String((const char *)(d["bbc"] | config.barBgColor.c_str()));
    config.goodColor = String((const char *)(d["gdc"] | config.goodColor.c_str()));
    config.warnColor = String((const char *)(d["wnc"] | config.warnColor.c_str()));
    config.badColor = String((const char *)(d["bdc"] | config.badColor.c_str()));
    config.disabledColor = String((const char *)(d["dsc"] | config.disabledColor.c_str()));
    config.trendWindowMin = d["twin"] | config.trendWindowMin;
    config.trendMarginPct = d["tmar"] | config.trendMarginPct;
    config.animSquarePx = d["asq"] | config.animSquarePx;
    config.alertThresholdPct = d["alrt"] | config.alertThresholdPct;
    config.claudeLabel = String((const char *)(d["clbl"] | config.claudeLabel.c_str()));
    config.codexLabel = String((const char *)(d["xlbl"] | config.codexLabel.c_str()));
    config.animStyle = d["asty"] | config.animStyle;
    config.animHz = d["ahz"] | config.animHz;
    config.alertBlinkMs = d["ablk"] | config.alertBlinkMs;
    sanitizeConfig();
    return true;
}

// ---------------------------------------------------------------------------
// Retroiluminacion.
// ---------------------------------------------------------------------------

// Aplica un brillo INMEDIATO (arranque, emergencia). Sincroniza el objetivo para que la rampa
// no lo mueva despues.
void applyBrightness(uint8_t percent)
{
    brightnessTargetPct = percent;
    if (percent == appliedBrightnessPct)
    {
        return;
    }
    appliedBrightnessPct = percent;
    const uint32_t duty = (uint32_t)percent * 255 / 100;
    ledcWrite(BL_LEDC_CHANNEL, duty);
}

// Converge appliedBrightnessPct hacia brightnessTargetPct unos puntos por vuelta del loop: la
// atenuacion/reactivacion se ve como una rampa suave (~200-300ms) en vez de un escalon duro.
void rampBrightness()
{
    if (appliedBrightnessPct == brightnessTargetPct)
    {
        return;
    }
    const int16_t step = 4;
    int16_t cur = appliedBrightnessPct;
    if (cur < brightnessTargetPct)
    {
        cur += step;
        if (cur > brightnessTargetPct)
        {
            cur = brightnessTargetPct;
        }
    }
    else
    {
        cur -= step;
        if (cur < brightnessTargetPct)
        {
            cur = brightnessTargetPct;
        }
    }
    appliedBrightnessPct = (uint8_t)cur;
    ledcWrite(BL_LEDC_CHANNEL, (uint32_t)appliedBrightnessPct * 255 / 100);
}

// ---------------------------------------------------------------------------
// Fuente de pixeles 3x5 (estilo 8-bit / terminal), portada del renderer.py del
// tema "delta" del repo OLED. Cada pixel de origen se dibuja como un bloque de
// scale x scale px, dando el aspecto pixelado. Los bits van MSB = columna izquierda.
// ---------------------------------------------------------------------------

struct PixelGlyph
{
    char c;
    uint8_t rows[5];
};

const PixelGlyph PIXEL_FONT[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}},
    {'3', {7, 1, 7, 1, 7}}, {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}},
    {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 2, 2, 2}}, {'8', {7, 5, 7, 5, 7}},
    {'9', {7, 5, 7, 1, 7}}, {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}},
    {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}}, {'E', {7, 4, 6, 4, 7}},
    {'F', {7, 4, 6, 4, 4}}, {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}},
    {'I', {7, 2, 2, 2, 7}}, {'J', {1, 1, 1, 5, 2}}, {'K', {5, 6, 4, 6, 5}},
    {'L', {4, 4, 4, 4, 7}}, {'M', {5, 7, 7, 5, 5}}, {'N', {7, 5, 5, 5, 5}},
    {'O', {7, 5, 5, 5, 7}}, {'P', {7, 5, 7, 4, 4}}, {'Q', {7, 5, 5, 7, 1}},
    {'R', {7, 5, 7, 6, 5}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}},
    {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}}, {'W', {5, 5, 7, 7, 5}},
    {'X', {5, 5, 2, 5, 5}}, {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}},
    {'%', {5, 1, 2, 4, 5}}, {':', {0, 2, 0, 2, 0}}, {'-', {0, 0, 7, 0, 0}},
    {'+', {0, 2, 7, 2, 0}}, {'>', {4, 2, 1, 2, 4}}, {'_', {0, 0, 0, 0, 7}},
    {'.', {0, 0, 0, 0, 2}}, {'/', {1, 1, 2, 4, 4}}, {'?', {7, 1, 3, 0, 2}},
    {'!', {2, 2, 2, 0, 2}}, {' ', {0, 0, 0, 0, 0}},
};
const uint8_t PIXEL_FONT_COUNT = sizeof(PIXEL_FONT) / sizeof(PIXEL_FONT[0]);

const uint8_t *glyphRows(char c)
{
    if (c >= 'a' && c <= 'z')
    {
        c -= 32; // La fuente es solo mayusculas.
    }
    for (uint8_t i = 0; i < PIXEL_FONT_COUNT; i++)
    {
        if (PIXEL_FONT[i].c == c)
        {
            return PIXEL_FONT[i].rows;
        }
    }
    return PIXEL_FONT[PIXEL_FONT_COUNT - 1].rows; // Espacio (ultimo) como fallback.
}

int16_t pixelTextWidth(const String &text, uint8_t scale)
{
    if (text.length() == 0)
    {
        return 0;
    }
    // Cada glifo avanza 4 columnas (3 + 1 de separacion); sin la ultima separacion.
    return (int16_t)((4 * (int)text.length() - 1) * scale);
}

// Las primitivas de dibujo se hacen plantilla (G) para poder renderizar tanto en la
// pantalla (tft) como en un sprite fuera de pantalla (panelSprite). En TFT_eSPI solo
// drawPixel es virtual, asi que con plantillas el despacho es estatico y correcto.
template <typename G>
void drawPixelText(G &g, int16_t x, int16_t y, const String &text, uint8_t scale, uint16_t color)
{
    int16_t cursor = x;
    for (size_t i = 0; i < text.length(); i++)
    {
        const uint8_t *rows = glyphRows(text.charAt(i));
        for (uint8_t r = 0; r < 5; r++)
        {
            const uint8_t bits = rows[r];
            for (uint8_t c = 0; c < 3; c++)
            {
                if (bits & (1 << (2 - c)))
                {
                    g.fillRect(cursor + c * scale, y + r * scale, scale, scale, color);
                }
            }
        }
        cursor += 4 * scale;
    }
}

template <typename G>
void drawPixelTextRight(G &g, int16_t rightX, int16_t y, const String &text, uint8_t scale, uint16_t color)
{
    drawPixelText(g, rightX - pixelTextWidth(text, scale), y, text, scale, color);
}

template <typename G>
void drawPixelTextCenter(G &g, int16_t cx, int16_t y, const String &text, uint8_t scale, uint16_t color)
{
    drawPixelText(g, cx - pixelTextWidth(text, scale) / 2, y, text, scale, color);
}

// ---------------------------------------------------------------------------
// Dibujo: barra de uso estilo "delta".
// ---------------------------------------------------------------------------

template <typename G>
void drawUsageBar(G &g, int16_t x, int16_t y, int16_t w, int16_t h, const WindowData &window, uint16_t barColor)
{
    // Barra "delta" en estilo 8-bit: relleno solido (del color del framework) hasta el %
    // real; el tramo entre el actual y lo esperado (regresion lineal) se marca con superavit
    // (franja de fondo de 2px arriba y abajo) o deficit (puntos dispersos, dithering).
    // Borde de 1px para que el area util sea mas grande.
    const float remaining = clamp01(window.remaining);
    const float expected = clamp01(window.expected);
    const uint16_t col = barColor;

    g.fillRect(x, y, w, h, pal.barBg);

    const int16_t innerX = x + 1;
    const int16_t innerY = y + 1;
    const int16_t innerW = w - 2;
    const int16_t innerH = h - 2;
    const int16_t actualW = (int16_t)(innerW * (remaining / 100.0f));
    const int16_t actualX = innerX + actualW;
    const int16_t expectedX = innerX + (int16_t)(innerW * (expected / 100.0f));

    if (actualW > 0)
    {
        g.fillRect(innerX, innerY, actualW, innerH, col);
    }

    if (actualX >= expectedX)
    {
        if (actualX > expectedX)
        {
            g.fillRect(expectedX, innerY, actualX - expectedX, 2, pal.barBg);
            g.fillRect(expectedX, innerY + innerH - 2, actualX - expectedX, 2, pal.barBg);
        }
    }
    else
    {
        const int16_t startPx = actualW > 0 ? actualX + 2 : innerX;
        for (int16_t px = startPx; px <= expectedX; px += 2)
        {
            for (int16_t py = innerY; py <= innerY + innerH - 1; py += 2)
            {
                g.drawPixel(px, py, col);
            }
        }
    }

    // Solo el borde exterior; sin la marca vertical blanca del limite.
    g.drawRect(x, y, w, h, pal.panelDivider);
}

bool isResetUnit(char c)
{
    return c == 'H' || c == 'D' || c == 'M';
}

// Ancho total del reset con digitos a digitScale y letras de unidad (H/D/M) a unitScale,
// mas el hueco extra tras la primera unidad (separa horas|minutos, dias|horas).
int16_t resetMixedWidth(const String &text, uint8_t digitScale, uint8_t unitScale, int16_t extraGapPx)
{
    int16_t w = 0;
    bool boundaryDone = false;
    for (size_t i = 0; i < text.length(); i++)
    {
        const uint8_t s = isResetUnit(text.charAt(i)) ? unitScale : digitScale;
        w += 3 * s; // glifo
        if (i + 1 < text.length())
        {
            w += s; // separacion normal
            if (isResetUnit(text.charAt(i)) && !boundaryDone)
            {
                w += extraGapPx;
                boundaryDone = true;
            }
        }
    }
    return w;
}

// Reset alineado a la derecha con las LETRAS de unidad (H/D/M) mas chicas que los digitos,
// para que no le quiten protagonismo al numero. Alineadas por su base (abajo) con los digitos.
template <typename G>
void drawResetMixed(G &g, int16_t rightX, int16_t topY, const String &text, uint8_t digitScale, uint8_t unitScale, uint16_t digitColor, uint16_t unitColor, int16_t extraGapPx)
{
    int16_t x = rightX - resetMixedWidth(text, digitScale, unitScale, extraGapPx);
    const int16_t baseBottom = topY + 5 * digitScale; // base comun (abajo de los digitos)
    bool boundaryDone = false;
    for (size_t i = 0; i < text.length(); i++)
    {
        const char c = text.charAt(i);
        const bool isUnit = isResetUnit(c);
        const uint8_t s = isUnit ? unitScale : digitScale;
        drawPixelText(g, x, baseBottom - 5 * s, String(c), s, isUnit ? unitColor : digitColor);
        x += 3 * s;
        if (i + 1 < text.length())
        {
            x += s;
            if (isResetUnit(c) && !boundaryDone)
            {
                x += extraGapPx;
                boundaryDone = true;
            }
        }
    }
}

// Indicador de tendencia del superavit: flecha arriba (subio), flecha abajo (bajo) o punto
// (dentro del margen). El sentido se calcula en recordTrends() comparando contra el pasado.
template <typename G>
void drawTrendIndicator(G &g, int16_t x, int16_t cy, int8_t trend, uint16_t accent)
{
    // Flecha real: cabeza tipo chevron (triangulo ancho) + asta (barra), con el acento de la
    // IA. La direccion la da la orientacion. Estable (trend==0) no dibuja nada.
    const int16_t w = 11;         // ancho de la cabeza
    const int16_t cx = x + w / 2; // centro horizontal
    const int16_t sh = 2;         // media anchura del asta (asta de 5 px)
    if (trend > 0) // sube: cabeza arriba, asta hacia abajo
    {
        g.fillTriangle(cx, cy - 6, x, cy, x + w, cy, accent);
        g.fillRect(cx - sh, cy - 2, sh * 2 + 1, 8, accent);
    }
    else if (trend < 0) // baja: cabeza abajo, asta hacia arriba
    {
        g.fillTriangle(cx, cy + 6, x, cy, x + w, cy, accent);
        g.fillRect(cx - sh, cy - 6, sh * 2 + 1, 8, accent);
    }
}

// Ancho total de "NUM%" con el numero a numScale y el simbolo % a pctScale (mas chico).
int16_t percentMixedWidth(const String &num, uint8_t numScale, uint8_t pctScale)
{
    return pixelTextWidth(num, numScale) + pctScale + pixelTextWidth("%", pctScale); // gap = pctScale
}

// Dibuja "NUM%" con el numero a numScale y el % a pctScale (mas chico), alineados por la base
// (abajo), separados por un gap proporcional. Devuelve el ancho total.
template <typename G>
int16_t drawPercentMixed(G &g, int16_t x, int16_t topY, const String &num, uint8_t numScale, uint8_t pctScale, uint16_t color)
{
    const int16_t baseBottom = topY + 5 * numScale; // base comun (abajo del numero grande)
    drawPixelText(g, x, topY, num, numScale, color);
    const int16_t px = x + pixelTextWidth(num, numScale) + pctScale;
    drawPixelText(g, px, baseBottom - 5 * pctScale, "%", pctScale, color); // % chico, pegado a la base
    return percentMixedWidth(num, numScale, pctScale);
}

// ---------------------------------------------------------------------------
// Dibujo: bloque de una ventana (linea de datos + barra).
// ---------------------------------------------------------------------------

template <typename G>
void drawWindowBlock(G &g, int16_t topY, const char *tag, const WindowData &window, uint16_t accent, int8_t trend)
{
    const int16_t marginX = 6;

    // Tag alineado a la base comun de la fila (topY+20), igual que %/delta/reset.
    drawPixelText(g, marginX, topY + 10, tag, 2, pal.textMuted);

    // Umbral de alerta: si el % restante cae al/por debajo del umbral, numero y barra en rojo.
    const int remaining = (int)lround(clamp01(window.remaining));
    const bool alert = (config.alertThresholdPct > 0 && remaining <= config.alertThresholdPct);
    const uint16_t mainColor = alert ? pal.bad : accent;
    // Numero grande (escala 4) con el simbolo % mas chico (escala 3), alineados por la base.
    drawPercentMixed(g, 34, topY, String(remaining), 4, 3, mainColor);

    // Delta (surplus): numero a escala 3 con el % a escala 2 (mas chico), centrado en el hueco
    // entre el % grande y el reset. Positivo con el color del framework, negativo en rojo.
    const int delta = (int)lround(window.remaining - window.expected);
    String deltaNum = formatDelta(window); // "+N%" o "-N%"
    if (deltaNum.endsWith("%"))
    {
        deltaNum = deltaNum.substring(0, deltaNum.length() - 1); // separa el % para dibujarlo chico
    }
    const int16_t deltaCx = 152;                                   // centro fijo (igual en 5H y WK)
    const int16_t deltaW = percentMixedWidth(deltaNum, 3, 2);      // ancho total (numero + % chico)
    drawPercentMixed(g, deltaCx - deltaW / 2, topY + 5, deltaNum, 3, 2, delta >= 0 ? accent : pal.bad);

    // Tendencia del superavit: flecha a la derecha del delta (nada si esta estable).
    if (trend != 0)
    {
        drawTrendIndicator(g, deltaCx + deltaW / 2 + 6, topY + 5 + 7, trend, accent);
    }

    // Reset a la derecha, separando horas|minutos (dias|horas) unos 4px.
    // Digitos a escala 3, letras de unidad (h/m/d) a escala 2 (mas chicas) pero en blanco
    // (textPrimary) igual que los digitos, no en gris.
    drawResetMixed(g, SCREEN_W - marginX, topY + 5, formatDuration(window.resetSeconds), 3, 2, pal.textPrimary, pal.textPrimary, 4);

    drawUsageBar(g, marginX, topY + 24, SCREEN_W - marginX * 2, 14, window, mainColor);
}

// Dibuja la animacion "busy" dentro de una caja (boxX,boxY,boxW,boxH) del destino g. Se usa
// tanto en el panel completo como en la franja pequena (animateBusyStrip), reutilizando la
// misma logica de estilos (marquee / pulso / barra).
template <typename G>
void drawBusyIndicator(G &g, int16_t boxX, int16_t boxY, int16_t boxW, int16_t boxH, uint16_t accent, uint8_t sessions)
{
    const int16_t boxRight = boxX + boxW;
    const int16_t cy = boxY + boxH / 2;
    const int16_t stripX = boxX + 2;
    const int16_t stripW = boxRight - stripX;
    if (config.animStyle == 1)
    {
        // Pulso: un circulo del acento que late (radio en onda triangular).
        const int16_t maxR = boxH / 2 - 1;
        const int16_t per = 60;
        const int16_t ph = (int16_t)(dotScroll % (uint32_t)per);
        const int16_t tri = ph < per / 2 ? ph : per - ph; // 0..30..0
        const int16_t r = 2 + tri * (maxR - 2) / (per / 2);
        g.fillCircle(boxRight - maxR - 2, cy, r, accent);
    }
    else if (config.animStyle == 2)
    {
        // Barra indeterminada: un segmento que barre el ancho de izquierda a derecha.
        const int16_t segW = stripW / 3;
        int16_t bx = stripX + (int16_t)(dotScroll % (uint32_t)(stripW + segW)) - segW;
        int16_t bw = segW;
        if (bx < stripX)
        {
            bw -= (stripX - bx);
            bx = stripX;
        }
        if (bx + bw > boxRight)
        {
            bw = boxRight - bx;
        }
        if (bw > 0)
        {
            g.fillRect(bx, cy - 3, bw, 6, accent);
        }
    }
    else
    {
        // 0 = marquee: cuadrados equiespaciados que fluyen y CRUZAN los bordes recortandose
        // (aparecen/desaparecen gradualmente al atravesar, no saltan). Cada cuadro se recorta a
        // [stripX, boxRight]: nunca se dibuja a la izquierda del area (evita la "linea pegada"
        // sobre el nombre) ni pasado el borde derecho.
        uint8_t n = sessions * 2;
        if (n < 2)
        {
            n = 2;
        }
        if (n > 18)
        {
            n = 18;
        }
        int16_t gap = stripW / n; // separacion uniforme
        if (gap < 1)
        {
            gap = 1; // blindaje: nunca 0 (evita modulo por cero)
        }
        const int16_t sq = config.animSquarePx;
        const int16_t phase = (int16_t)(dotScroll % (uint32_t)gap); // desfase continuo dentro de un hueco
        for (int16_t x = phase - gap; x <= stripW + sq; x += gap)
        {
            const int16_t left = stripX + x - sq / 2;
            const int16_t l = left < stripX ? stripX : left;                   // recorta al borde izq
            const int16_t r = (left + sq > boxRight) ? boxRight : (left + sq); // y al borde der
            if (r > l)
            {
                g.fillRect(l, cy - sq / 2, r - l, sq, accent);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Dibujo: indicador de actividad (recuadro superior derecho del panel).
// ---------------------------------------------------------------------------

template <typename G>
void drawActivityIndicator(G &g, const ToolData &tool, uint16_t accent, int16_t panelTop, uint8_t sessions)
{
    const int16_t boxX = config.animBoxX; // borde izquierdo configurable del area de animacion
    const int16_t boxRight = SCREEN_W - 6;
    const int16_t boxY = panelTop + 4;
    const int16_t boxW = boxRight - boxX;
    const int16_t boxH = 18;
    g.fillRect(boxX, boxY, boxW, boxH, pal.bg);

    const int16_t cy = boxY + boxH / 2;
    const int16_t labelY = boxY + 4;
    const int16_t cx = boxX + 8;

    if (tool.activity == "busy")
    {
        drawBusyIndicator(g, boxX, boxY, boxW, boxH, accent, sessions);
    }
    else if (tool.activity == "idle")
    {
        // Punto + "IDLE" agrupados y alineados a la derecha (antes el punto quedaba solo a la
        // izquierda con un vacio grande en medio).
        const int16_t textRight = SCREEN_W - 6;
        const int16_t textLeft = textRight - pixelTextWidth("IDLE", 2);
        g.drawRect(textLeft - 12, cy - 3, 6, 6, pal.muted);
        drawPixelTextRight(g, textRight, labelY, "IDLE", 2, pal.muted);
    }
    // waiting: no se dibuja nada aqui; el panel entero se invierte para llamar la atencion.
}

// ---------------------------------------------------------------------------
// Dibujo: logo de cada herramienta (Claude "spark" / Codex nube-terminal).
// ---------------------------------------------------------------------------

template <typename G>
void drawToolIcon(G &g, bool isClaude, int16_t cx, int16_t cy, uint16_t fg)
{
    if (isClaude)
    {
        // "Spark" de Claude: sunburst ASIMETRICO de espigas afiladas. Angulos irregulares y
        // longitudes distintas (no un asterisco simetrico), como el logo real.
        static const float rays[][2] = {
            {-15, 8.5f}, {18, 11.0f}, {45, 6.5f}, {78, 11.5f}, {108, 8.0f}, {140, 10.5f},
            {168, 6.0f}, {200, 11.0f}, {232, 8.0f}, {262, 11.5f}, {295, 7.0f}, {330, 10.0f},
        };
        const float baseHalf = 1.3f;
        for (uint8_t i = 0; i < sizeof(rays) / sizeof(rays[0]); i++)
        {
            const float angle = rays[i][0] * (PI / 180.0f);
            const float len = rays[i][1];
            const float perp = angle + PI / 2.0f;
            const int16_t apexX = cx + (int16_t)round(cos(angle) * len);
            const int16_t apexY = cy + (int16_t)round(sin(angle) * len);
            const int16_t b1X = cx + (int16_t)round(cos(perp) * baseHalf);
            const int16_t b1Y = cy + (int16_t)round(sin(perp) * baseHalf);
            const int16_t b2X = cx - (int16_t)round(cos(perp) * baseHalf);
            const int16_t b2Y = cy - (int16_t)round(sin(perp) * baseHalf);
            g.fillTriangle(apexX, apexY, b1X, b1Y, b2X, b2Y, fg);
        }
        g.fillCircle(cx, cy, 2, fg);
    }
    else
    {
        // Codex: prompt ">_" (sin nube), DELINEADO con trazo de ancho constante (2px). El
        // chevron son dos segmentos (cada uno duplicado 1px) y el guion bajo un rectangulo.
        for (int8_t o = 0; o < 2; o++)
        {
            g.drawLine(cx - 6, cy - 6 + o, cx + 1, cy + o, fg); // trazo superior
            g.drawLine(cx + 1, cy + o, cx - 6, cy + 6 + o, fg); // trazo inferior
        }
        g.fillRect(cx + 2, cy + 5, 9, 2, fg); // guion bajo "_"
    }
}

// ---------------------------------------------------------------------------
// Dibujo: panel completo de una herramienta (en el sprite, coordenadas locales).
// ---------------------------------------------------------------------------

// Nombre a mostrar de una herramienta: alias local configurado o, si esta vacio, el del servicio.
String effectiveLabel(bool isClaude, const ToolData &tool)
{
    const String &ov = isClaude ? config.claudeLabel : config.codexLabel;
    return ov.length() > 0 ? ov : tool.label;
}

template <typename G>
void drawToolPanel(G &g, const ToolData &tool, bool isClaude, int16_t panelTop)
{
    const uint16_t accent = isClaude ? pal.claude : pal.codex;

    g.fillRect(0, panelTop, SCREEN_W, PANEL_H, pal.bg);

    String label = effectiveLabel(isClaude, tool);
    label.toUpperCase();
    drawToolIcon(g, isClaude, 17, panelTop + 13, accent);
    drawPixelText(g, 38, panelTop + 6, label, 3, accent);

    const uint8_t sessions = isClaude ? claudeSessions : codexSessions;

    if (!tool.enabled)
    {
        // Desactivado: pantalla limpia, sin el indicador de actividad "IDLE" (era contradictorio).
        drawPixelTextCenter(g, SCREEN_W / 2, panelTop + PANEL_H / 2, "DESACTIVADO", 2, pal.muted);
        return;
    }

    g.drawFastHLine(6, panelTop + 26, SCREEN_W - 12, pal.panelDivider);

    const uint8_t trendBase = isClaude ? 0 : 2;
    drawWindowBlock(g, panelTop + 32, "5H", tool.current, accent, trendDir[trendBase + 0]);
    drawWindowBlock(g, panelTop + 74, "WK", tool.weekly, accent, trendDir[trendBase + 1]);

    drawActivityIndicator(g, tool, accent, panelTop, sessions);
}

// ---------------------------------------------------------------------------
// Pantallas auxiliares (arranque / offline / AP). Se dibujan directo en la pantalla.
// ---------------------------------------------------------------------------

void drawCenteredMessage(const String &title, const String &subtitle, uint16_t titleColor)
{
    tft.fillScreen(pal.bg);
    drawPixelTextCenter(tft, SCREEN_W / 2, SCREEN_H / 2 - 26, title, 3, titleColor);
    // Baja a escala 1 si el subtitulo (p.ej. una URL o error HTTP) no cabe a escala 2.
    const uint8_t ss = pixelTextWidth(subtitle, 2) > SCREEN_W - 16 ? 1 : 2;
    drawPixelTextCenter(tft, SCREEN_W / 2, SCREEN_H / 2 + 6, subtitle, ss, pal.textMuted);
}

// Pantalla de arranque con titulo fijo y subtitulo actualizable SIN repintar todo (anti
// flicker: el titulo se pinta una vez, el bucle solo refresca la banda del subtitulo).
void drawStartupTitle(const String &title)
{
    tft.fillScreen(pal.bg);
    drawPixelTextCenter(tft, SCREEN_W / 2, SCREEN_H / 2 - 26, title, 3, pal.claude);
}
void drawStartupSubtitle(const String &subtitle)
{
    tft.fillRect(0, SCREEN_H / 2 + 2, SCREEN_W, 20, pal.bg);
    drawPixelTextCenter(tft, SCREEN_W / 2, SCREEN_H / 2 + 6, subtitle, 2, pal.textMuted);
}

void drawOfflineScreen(const String &reason)
{
    drawCenteredMessage("Sin servicio", reason, pal.warn);
    // No re-armar needsFullRedraw: se dibuja solo en la transicion a offline, no en cada
    // publicacion (evita el repintado completo repetido y su parpadeo).
    needsFullRedraw = false;
}

void drawApScreen()
{
    tft.fillScreen(pal.bg);
    drawPixelTextCenter(tft, SCREEN_W / 2, 24, "MODO CONFIG", 3, pal.warn);
    drawPixelTextCenter(tft, SCREEN_W / 2, 74, "CONECTATE AL WIFI", 2, pal.textPrimary);
    drawPixelTextCenter(tft, SCREEN_W / 2, 100, EMERGENCY_AP_SSID, 2, pal.claude);
    drawPixelTextCenter(tft, SCREEN_W / 2, 128, String("CLAVE: ") + EMERGENCY_AP_PASSWORD, 2, pal.textMuted);
    drawPixelTextCenter(tft, SCREEN_W / 2, 168, "LUEGO ABRE", 2, pal.textPrimary);
    drawPixelTextCenter(tft, SCREEN_W / 2, 194, EMERGENCY_AP_URL, 2, pal.codex);
}

// ---------------------------------------------------------------------------
// Consola de arranque: logs de inicio en pantalla (POST, WiFi, servicios) + serial.
// ---------------------------------------------------------------------------

const uint8_t BOOT_LOG_MAX = 15; // cuantas lineas caben en pantalla a escala 2
String bootLogLines[BOOT_LOG_MAX];
uint16_t bootLogColors[BOOT_LOG_MAX];
uint8_t bootLogCount = 0;

// Buffer de logs para verlos en tiempo real desde la web (/api/logs). Se escribe SOLO
// desde el core1 (setup + loop + handlers web, todos secuenciales), asi que no hace falta
// mutex; la tarea de red (core0) NUNCA llama a logLine.
const uint16_t LOG_MAX_LEN = 8000; // buffer RAM para la vista en vivo (mas historia)
String logBuffer = "";

// --- Logging a SD asincrono (fuera del path de render) ---
// logAt() corre en core1 (el core de render). Escribir a la SD (SD.open/print/close, ~10-50ms)
// ahi hincaba el render en cada transicion/actividad. En su lugar, logAt encola la linea y una
// tarea dedicada (core0) la vuelca a la SD. Como los handlers web tambien tocan la SD (en core1),
// TODO acceso a SD se serializa con sdMutex via el guard RAII SdGuard (libera en cualquier return).
SemaphoreHandle_t sdMutex = nullptr;
struct SdGuard
{
    SdGuard()
    {
        if (sdMutex)
        {
            xSemaphoreTake(sdMutex, portMAX_DELAY);
        }
    }
    ~SdGuard()
    {
        if (sdMutex)
        {
            xSemaphoreGive(sdMutex);
        }
    }
};
const uint16_t LOG_LINE_MAX = 160;  // tope por linea encolada (se trunca; el buffer RAM guarda la completa)
const uint8_t LOG_QUEUE_DEPTH = 24; // suficiente para todas las lineas de arranque sin descartar
struct LogEntry
{
    char text[LOG_LINE_MAX];
};
QueueHandle_t logQueue = nullptr;
volatile uint32_t logDropped = 0; // lineas descartadas por cola llena (solo afecta la copia en SD)

// ID unico de arranque: 6 alfanumericos en formato XXX-XXX. Va en cada linea de log para
// poder separar los logs de la SD por # de arranque.
String bootId = "000-000";
uint32_t bootCount = 0; // # consecutivo de arranque (persistido en NVS)
String makeBootId()
{
    static const char CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; // 36
    char c[6];
    for (uint8_t i = 0; i < 6; i++)
    {
        c[i] = CHARS[esp_random() % 36];
    }
    char out[8] = {c[0], c[1], c[2], '-', c[3], c[4], c[5], 0};
    return String(out);
}

enum LogLevel
{
    LVL_DEBUG = 0,
    LVL_INFO = 1,
    LVL_WARN = 2,
    LVL_ERROR = 3
};
const char *const LVL_NAMES[] = {"DEBUG", "INFO", "WARN", "ERROR"};

// Timestamp real "YYYY-MM-DD HH:MM:SS.mmm" (hora local con milisegundos; se fija con NTP
// tras conectar WiFi). Antes de sincronizar es 1970-01-01 + uptime (igual parseable).
String nowTimestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const time_t now = tv.tv_sec;
    struct tm ti;
    localtime_r(&now, &ti);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &ti);
    char out[30];
    snprintf(out, sizeof(out), "%s.%03d", b, (int)(tv.tv_usec / 1000));
    return String(out);
}

// Append a la SD con rotacion de LOG_FILE_KEEP archivos de LOG_FILE_MAX_BYTES cada uno.
// Solo lo llama la tarea logger (core0). Serializa el acceso a SD con el resto (handlers web).
void sdLogAppend(const String &line)
{
    SdGuard _sd; // serializa contra los accesos a SD de los handlers web (core1)
    File f = SD.open(LOG_FILE_PATH, FILE_APPEND);
    if (!f)
    {
        return;
    }
    f.println(line);
    const bool rotate = f.size() > LOG_FILE_MAX_BYTES;
    f.close();
    if (rotate)
    {
        SD.remove(String("/cyd-log.") + String(LOG_FILE_KEEP) + ".txt");
        for (int i = LOG_FILE_KEEP - 1; i >= 1; i--)
        {
            SD.rename(String("/cyd-log.") + String(i) + ".txt", String("/cyd-log.") + String(i + 1) + ".txt");
        }
        SD.rename(LOG_FILE_PATH, "/cyd-log.1.txt");
    }
}

// Tarea dedicada (core0): drena la cola de logs y los vuelca a la SD, fuera del path de render.
void loggerTask(void *param)
{
    (void)param;
    LogEntry e;
    for (;;)
    {
        if (xQueueReceive(logQueue, &e, portMAX_DELAY) == pdTRUE && sdReady)
        {
            sdLogAppend(String(e.text));
        }
    }
}

// Log con nivel + timestamp. Se escribe a serial y al buffer RAM (vista web) en el acto, y se ENCOLA
// para que la tarea logger lo persista a la SD sin bloquear el render. Solo se llama desde core1
// (serial/RAM secuencial); la cola es thread-safe hacia la tarea logger (core0).
void logAt(LogLevel level, const String &msg)
{
    // Formato: "[<fecha>] [<bootId>] [<NIVEL>] <mensaje>".
    const String line = "[" + nowTimestamp() + "] [" + bootId + "] [" + LVL_NAMES[level] + "] " + msg;
    Serial.println(line);
    logBuffer += line + "\n";
    const int over = (int)logBuffer.length() - LOG_MAX_LEN;
    if (over > 0)
    {
        logBuffer.remove(0, over);
    }
    if (logQueue)
    {
        LogEntry e;
        strlcpy(e.text, line.c_str(), sizeof(e.text));
        if (xQueueSend(logQueue, &e, 0) != pdTRUE) // no bloquea el render: si la cola esta llena, descarta
        {
            logDropped++; // la linea sigue en el buffer RAM y en serial; solo se pierde en la copia de SD
        }
    }
}

// Compat: los logs sin nivel explicito son INFO.
void logLine(const String &msg)
{
    logAt(LVL_INFO, msg);
}

const int16_t BOOT_LINE0_Y = 34; // Y de la primera linea de log
const int16_t BOOT_LINE_H = 14;  // alto de linea

// Cabecera de la consola (fondo + titulo + divisor). Se pinta UNA vez.
void drawBootHeader()
{
    tft.fillScreen(pal.bg);
    drawPixelText(tft, 8, 4, ">_ CYD USAGE MONITOR", 3, pal.claude); // escala 3 (coherente con las otras pantallas)
    tft.drawFastHLine(8, 28, SCREEN_W - 16, pal.panelDivider);
}

// Redibujo completo (solo se usa cuando el ring hace scroll y todas las lineas se desplazan).
void drawBootConsole()
{
    drawBootHeader();
    int16_t y = BOOT_LINE0_Y;
    for (uint8_t i = 0; i < bootLogCount; i++)
    {
        drawPixelText(tft, 8, y, bootLogLines[i], 2, bootLogColors[i]);
        y += BOOT_LINE_H;
    }
}

void bootLog(const String &msg, uint16_t color)
{
    // El color del boot mapea al nivel de log: rojo=ERROR, ambar=WARN, resto INFO.
    const LogLevel lvl = (color == pal.bad) ? LVL_ERROR : (color == pal.warn) ? LVL_WARN : LVL_INFO;
    logAt(lvl, msg);
    if (bootLogCount == 0)
    {
        drawBootHeader(); // primera linea: pinta la cabecera una sola vez (anti flicker)
    }
    if (bootLogCount < BOOT_LOG_MAX)
    {
        bootLogLines[bootLogCount] = msg;
        bootLogColors[bootLogCount] = color;
        // Dibuja SOLO la linea nueva (sin fillScreen): elimina el estroboscopio del arranque.
        drawPixelText(tft, 8, BOOT_LINE0_Y + bootLogCount * BOOT_LINE_H, msg, 2, color);
        bootLogCount++;
    }
    else
    {
        // Ring lleno: todo se desplaza -> redibujo completo (caso raro, solo si hay >15 lineas).
        for (uint8_t i = 1; i < BOOT_LOG_MAX; i++)
        {
            bootLogLines[i - 1] = bootLogLines[i];
            bootLogColors[i - 1] = bootLogColors[i];
        }
        bootLogLines[BOOT_LOG_MAX - 1] = msg;
        bootLogColors[BOOT_LOG_MAX - 1] = color;
        drawBootConsole();
    }
}

// Reemplaza la ultima linea (progreso, p.ej. contador WiFi): limpia solo su banda y redibuja.
void bootLogUpdate(const String &msg, uint16_t color)
{
    if (bootLogCount == 0)
    {
        bootLog(msg, color);
        return;
    }
    bootLogLines[bootLogCount - 1] = msg;
    bootLogColors[bootLogCount - 1] = color;
    const int16_t y = BOOT_LINE0_Y + (bootLogCount - 1) * BOOT_LINE_H;
    tft.fillRect(8, y, SCREEN_W - 16, 11, pal.bg);
    drawPixelText(tft, 8, y, msg, 2, color);
}

// ---------------------------------------------------------------------------
// Screensavers (anti burn-in cuando no hay servicio). A color, 320x240, dibujo directo.
// 0 off, 1 matrix, 2 dvd, 3 stars, 4 snake.
// ---------------------------------------------------------------------------

const uint32_t SAVER_FRAME_MS = 40;
const uint32_t SAVER_GRACE_MS = 12000; // gracia offline antes de lanzar el screensaver

// Matrix
const int16_t MTX_CELL_W = 6;
const int16_t MTX_CELL_H = 8;
const int16_t MTX_COLS = SCREEN_W / MTX_CELL_W;
const int16_t MTX_ROWS = SCREEN_H / MTX_CELL_H;
int16_t mtxHead[MTX_COLS];

// DVD
const int16_t DVD_W = 96;
const int16_t DVD_H = 46;
int16_t dvdX, dvdY, dvdVX, dvdVY;
uint16_t dvdColor;
TFT_eSprite dvdSprite = TFT_eSprite(&tft); // el logo se dibuja aqui y se vuelca atomico (sin flicker)
bool dvdSpriteReady = false;

// Stars (warp radial)
const uint8_t STAR_N = 72;
int16_t starPX[STAR_N], starPY[STAR_N];
float starAng[STAR_N], starRad[STAR_N], starSpd[STAR_N];

// Snake (auto)
const int16_t SNK_CELL = 8;
const int16_t SNK_COLS = SCREEN_W / SNK_CELL;
const int16_t SNK_ROWS = SCREEN_H / SNK_CELL;
const uint16_t SNK_MAX = 220;
int16_t snkX[SNK_MAX], snkY[SNK_MAX];
uint16_t snkLen;
int8_t snkDX, snkDY;
int16_t foodX, foodY;

bool saverActive = false;
uint8_t saverRunningId = 0;
uint32_t lastSaverFrameMs = 0;
uint32_t offlineSinceMs = 0; // momento en que se dejo de tener servicio (para la gracia)
// Preview de screensaver disparado desde la web (fuerza el saver aunque haya servicio).
uint8_t saverPreviewId = 0;
uint32_t saverPreviewUntilMs = 0;

uint16_t saverRandColor()
{
    switch (random(6))
    {
    case 0: return tft.color565(255, 80, 80);
    case 1: return tft.color565(80, 235, 130);
    case 2: return tft.color565(90, 160, 235);
    case 3: return tft.color565(232, 175, 55);
    case 4: return tft.color565(205, 95, 225);
    default: return tft.color565(60, 220, 220);
    }
}

char saverRandGlyph()
{
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    return set[random(sizeof(set) - 1)];
}

void snakePlaceFood()
{
    foodX = random(SNK_COLS);
    foodY = random(SNK_ROWS);
    tft.fillRect(foodX * SNK_CELL, foodY * SNK_CELL, SNK_CELL - 1, SNK_CELL - 1, tft.color565(230, 70, 70));
}

void initSaver(uint8_t id)
{
    tft.fillScreen(pal.bg);
    if (id == 1)
    {
        for (int16_t i = 0; i < MTX_COLS; i++)
        {
            mtxHead[i] = -(int16_t)random(0, MTX_ROWS);
        }
    }
    else if (id == 2)
    {
        dvdX = 40;
        dvdY = 40;
        dvdVX = 3;
        dvdVY = 2;
        dvdColor = saverRandColor();
    }
    else if (id == 3)
    {
        for (uint8_t i = 0; i < STAR_N; i++)
        {
            starAng[i] = random(0, 628) / 100.0f;
            starRad[i] = random(1, 40);
            starSpd[i] = random(4, 16) / 40.0f;
            starPX[i] = -1;
        }
    }
    else if (id == 4)
    {
        snkLen = 5;
        for (uint16_t i = 0; i < snkLen; i++)
        {
            snkX[i] = SNK_COLS / 2 - i;
            snkY[i] = SNK_ROWS / 2;
        }
        snkDX = 1;
        snkDY = 0;
        snakePlaceFood();
    }
}

void stepMatrix()
{
    const uint16_t head = tft.color565(150, 255, 170);
    const uint16_t trail = tft.color565(25, 130, 60);
    for (int16_t c = 0; c < MTX_COLS; c++)
    {
        const int16_t h = mtxHead[c];
        const int16_t x = c * MTX_CELL_W;
        if (h - 1 >= 0 && h - 1 < MTX_ROWS)
        {
            tft.fillRect(x, (h - 1) * MTX_CELL_H, MTX_CELL_W, MTX_CELL_H, pal.bg);
            drawPixelText(tft, x + 1, (h - 1) * MTX_CELL_H + 1, String(saverRandGlyph()), 1, trail);
        }
        if (h >= 0 && h < MTX_ROWS)
        {
            tft.fillRect(x, h * MTX_CELL_H, MTX_CELL_W, MTX_CELL_H, pal.bg);
            drawPixelText(tft, x + 1, h * MTX_CELL_H + 1, String(saverRandGlyph()), 1, head);
        }
        const int16_t tail = h - 11;
        if (tail >= 0 && tail < MTX_ROWS)
        {
            tft.fillRect(x, tail * MTX_CELL_H, MTX_CELL_W, MTX_CELL_H, pal.bg);
        }
        mtxHead[c]++;
        if (mtxHead[c] > MTX_ROWS + 11)
        {
            mtxHead[c] = -(int16_t)random(0, 16);
        }
    }
}

// --- Wordmark "DVD" estilo logo (bold italico, trazos gruesos con panza curva) ---
// Todo se dibuja EN EL SPRITE (dvdSprite) para volcar atomico y evitar flicker.
// Trazo grueso (paralelogramo) entre dos puntos, como 2 triangulos.
void dvdThick(int16_t ax, int16_t ay, int16_t bx, int16_t by, float ht, uint16_t color)
{
    const float dx = bx - ax, dy = by - ay;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f)
    {
        return;
    }
    const int16_t px = (int16_t)(-dy / len * ht), py = (int16_t)(dx / len * ht);
    dvdSprite.fillTriangle(ax + px, ay + py, ax - px, ay - py, bx + px, by + py, color);
    dvdSprite.fillTriangle(ax - px, ay - py, bx - px, by - py, bx + px, by + py, color);
}
// Trazo en coordenadas locales de una letra (origen ox/topY), con corte italico (la parte de
// arriba se desplaza a la derecha). H=alto de letra, SL=italico, T=media anchura de trazo.
void dvdStroke(float ox, int16_t topY, float lx0, float ly0, float lx1, float ly1, uint16_t color)
{
    const float H = 24.0f, SL = 0.30f, T = 2.6f;
    const int16_t ax = (int16_t)(ox + lx0 + SL * (H - ly0)), ay = (int16_t)(topY + ly0);
    const int16_t bx = (int16_t)(ox + lx1 + SL * (H - ly1)), by = (int16_t)(topY + ly1);
    dvdThick(ax, ay, bx, by, T, color);
}
void dvdLetterD(float ox, int16_t topY, uint16_t c)
{
    const float W = 16.0f, H = 24.0f;
    dvdStroke(ox, topY, 0, 0, 0, H, c);                 // asta vertical izquierda
    dvdStroke(ox, topY, 0, 0, W * 0.55f, 0, c);         // trazo superior
    dvdStroke(ox, topY, 0, H, W * 0.55f, H, c);         // trazo inferior
    dvdStroke(ox, topY, W * 0.55f, 0, W, H * 0.32f, c); // panza sup-der
    dvdStroke(ox, topY, W, H * 0.32f, W, H * 0.68f, c); // panza der
    dvdStroke(ox, topY, W, H * 0.68f, W * 0.55f, H, c); // panza inf-der
}
void dvdLetterV(float ox, int16_t topY, uint16_t c)
{
    const float W = 16.0f, H = 24.0f;
    dvdStroke(ox, topY, 0, 0, W * 0.5f, H, c);
    dvdStroke(ox, topY, W * 0.5f, H, W, 0, c);
}
void drawDvdWordmark(int16_t leftX, int16_t topY, uint16_t c)
{
    const float step = 15.0f; // avance entre letras (solapan un poco, como el logo)
    dvdLetterD(leftX, topY, c);
    dvdLetterV(leftX + step, topY, c);
    dvdLetterD(leftX + 2 * step, topY, c);
}

void stepDvd()
{
    ensureSprite(dvdSprite, dvdSpriteReady, DVD_W, DVD_H, 16); // reserva bajo demanda (screensaver)
    const int16_t oldX = dvdX, oldY = dvdY;
    dvdX += dvdVX;
    dvdY += dvdVY;
    bool hit = false;
    if (dvdX <= 0) { dvdX = 0; dvdVX = -dvdVX; hit = true; }
    if (dvdX + DVD_W >= SCREEN_W) { dvdX = SCREEN_W - DVD_W; dvdVX = -dvdVX; hit = true; }
    if (dvdY <= 0) { dvdY = 0; dvdVY = -dvdVY; hit = true; }
    if (dvdY + DVD_H >= SCREEN_H) { dvdY = SCREEN_H - DVD_H; dvdVY = -dvdVY; hit = true; }
    if (hit)
    {
        dvdColor = saverRandColor(); // cambia de color al chocar con un borde
    }
    if (!dvdSpriteReady)
    {
        return; // sin memoria para el sprite: no dibuja (mejor nada que flicker)
    }
    // Logo en el sprite (coords locales): wordmark "DVD" bold italico + disco con agujero.
    dvdSprite.fillSprite(pal.bg);
    const int16_t cx = DVD_W / 2, discCy = 36;
    dvdSprite.fillEllipse(cx, discCy, DVD_W / 2 - 2, 6, dvdColor);
    dvdSprite.fillEllipse(cx, discCy, 7, 3, pal.bg);
    drawDvdWordmark(22, 2, dvdColor);
    // Borra SOLO la estela (la franja que quedo atras); no toca donde va el logo, asi el
    // volcado del sprite reemplaza el logo viejo por el nuevo sin pasar por negro = sin flicker.
    const int16_t dx = dvdX - oldX, dy = dvdY - oldY;
    if (dx > 0) { tft.fillRect(oldX, oldY, dx, DVD_H, pal.bg); }
    else if (dx < 0) { tft.fillRect(dvdX + DVD_W, oldY, -dx, DVD_H, pal.bg); }
    if (dy > 0) { tft.fillRect(oldX, oldY, DVD_W, dy, pal.bg); }
    else if (dy < 0) { tft.fillRect(oldX, dvdY + DVD_H, DVD_W, -dy, pal.bg); }
    dvdSprite.pushSprite(dvdX, dvdY);
}

void stepStars()
{
    const int16_t cx = SCREEN_W / 2;
    const int16_t cy = SCREEN_H / 2;
    for (uint8_t i = 0; i < STAR_N; i++)
    {
        if (starPX[i] >= 0)
        {
            tft.drawPixel(starPX[i], starPY[i], pal.bg);
        }
        starRad[i] += starSpd[i] * starRad[i] * 0.12f + 0.5f;
        const int16_t x = cx + (int16_t)(cos(starAng[i]) * starRad[i]);
        const int16_t y = cy + (int16_t)(sin(starAng[i]) * starRad[i]);
        if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
        {
            starAng[i] = random(0, 628) / 100.0f;
            starRad[i] = random(1, 10);
            starSpd[i] = random(4, 16) / 40.0f;
            starPX[i] = -1;
            continue;
        }
        const uint8_t b = (uint8_t)constrain((int)(starRad[i] * 1.4f), 50, 255);
        tft.drawPixel(x, y, tft.color565(b, b, b));
        if (starRad[i] > 90)
        {
            tft.drawPixel(x + 1, y, tft.color565(b, b, b)); // mas gruesa cuando esta cerca
        }
        starPX[i] = x;
        starPY[i] = y;
    }
}

void stepSnake()
{
    const int16_t hx = snkX[0];
    const int16_t hy = snkY[0];
    // Direccion greedy hacia la comida, evitando la reversa inmediata.
    const int8_t towardX = (foodX > hx) ? 1 : (foodX < hx ? -1 : 0);
    const int8_t towardY = (foodY > hy) ? 1 : (foodY < hy ? -1 : 0);
    int8_t ndx = snkDX;
    int8_t ndy = snkDY;
    if (towardX != 0 && !(towardX == -snkDX && snkDY == 0) && abs(foodX - hx) >= abs(foodY - hy))
    {
        ndx = towardX;
        ndy = 0;
    }
    else if (towardY != 0 && !(towardY == -snkDY && snkDX == 0))
    {
        ndx = 0;
        ndy = towardY;
    }
    else if (towardX != 0 && !(towardX == -snkDX && snkDY == 0))
    {
        ndx = towardX;
        ndy = 0;
    }
    snkDX = ndx;
    snkDY = ndy;
    int16_t nx = hx + snkDX;
    int16_t ny = hy + snkDY;
    if (nx < 0) { nx = SNK_COLS - 1; }
    if (nx >= SNK_COLS) { nx = 0; }
    if (ny < 0) { ny = SNK_ROWS - 1; }
    if (ny >= SNK_ROWS) { ny = 0; }
    for (uint16_t i = 0; i < snkLen; i++)
    {
        if (snkX[i] == nx && snkY[i] == ny)
        {
            initSaver(4); // choque consigo misma: reinicia
            return;
        }
    }
    const bool grew = (nx == foodX && ny == foodY);
    if (!grew)
    {
        tft.fillRect(snkX[snkLen - 1] * SNK_CELL, snkY[snkLen - 1] * SNK_CELL, SNK_CELL, SNK_CELL, pal.bg);
    }
    else if (snkLen < SNK_MAX)
    {
        snkLen++;
    }
    for (uint16_t i = snkLen - 1; i > 0; i--)
    {
        snkX[i] = snkX[i - 1];
        snkY[i] = snkY[i - 1];
    }
    snkX[0] = nx;
    snkY[0] = ny;
    tft.fillRect(hx * SNK_CELL, hy * SNK_CELL, SNK_CELL - 1, SNK_CELL - 1, tft.color565(40, 150, 70));
    tft.fillRect(nx * SNK_CELL, ny * SNK_CELL, SNK_CELL - 1, SNK_CELL - 1, tft.color565(90, 230, 120));
    if (grew)
    {
        snakePlaceFood();
    }
}

// Ejecuta el screensaver (offline, o el preview forzado desde la web). El preview tiene
// prioridad y muestra el saver aunque haya servicio, por unos segundos.
void runScreensaver()
{
    const bool preview = (saverPreviewId != 0 && millis() < saverPreviewUntilMs);
    const uint8_t id = preview ? saverPreviewId : config.screensaver;
    if (id == 0)
    {
        return;
    }
    if (!saverActive || saverRunningId != id)
    {
        initSaver(id);
        saverActive = true;
        saverRunningId = id;
    }
    if (millis() - lastSaverFrameMs < SAVER_FRAME_MS)
    {
        return;
    }
    lastSaverFrameMs = millis();
    switch (saverRunningId)
    {
    case 1: stepMatrix(); break;
    case 2: stepDvd(); break;
    case 3: stepStars(); break;
    case 4: stepSnake(); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Render de un panel a un sprite fuera de pantalla y volcado (sin flicker).
// ---------------------------------------------------------------------------

void renderPanel(bool isClaude)
{
    const ToolData &tool = isClaude ? monitorState.claude : monitorState.codex;
    const int16_t screenY = isClaude ? 0 : PANEL_H;

    drawToolPanel(panelSprite, tool, isClaude, 0);

    if (!isClaude)
    {
        panelSprite.drawFastHLine(0, 0, SCREEN_W, pal.panelDivider); // divisor entre paneles
    }
    panelSprite.pushSprite(0, screenY);
}

// Firma de los datos VISIBLES de una herramienta (cambia solo cuando cambia algo que se dibuja).
// resetSeconds es estatico entre snapshots, asi que no provoca repintados por segundo.
uint32_t prevClaudeSig = 0;
uint32_t prevCodexSig = 0;
uint32_t toolSig(const ToolData &t, uint8_t sessions, int8_t trend0, int8_t trend1)
{
    uint32_t h = 2166136261u;
    const uint32_t vals[] = {
        (uint32_t)lround(t.current.remaining * 10), (uint32_t)lround(t.current.expected * 10), (uint32_t)t.current.resetSeconds,
        (uint32_t)lround(t.weekly.remaining * 10), (uint32_t)lround(t.weekly.expected * 10), (uint32_t)t.weekly.resetSeconds,
        (uint32_t)(t.enabled ? 1 : 0), (uint32_t)sessions, (uint32_t)(uint8_t)trend0, (uint32_t)(uint8_t)trend1};
    for (uint8_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
    {
        h = (h ^ vals[i]) * 16777619u;
    }
    for (size_t i = 0; i < t.activity.length(); i++)
    {
        h = (h ^ (uint8_t)t.activity[i]) * 16777619u;
    }
    for (size_t i = 0; i < t.label.length(); i++)
    {
        h = (h ^ (uint8_t)t.label[i]) * 16777619u;
    }
    return h;
}

void renderMonitor()
{
    if (!monitorState.online)
    {
        return; // La pantalla offline ya se dibujo al detectar la caida.
    }
    if (!spriteReady)
    {
        // Sin sprite no hay render sin flicker; se avisa una vez en vez de dejar todo negro.
        if (needsFullRedraw)
        {
            drawCenteredMessage("SIN MEMORIA", "REINICIA EL CYD", pal.bad);
            needsFullRedraw = false;
        }
        return;
    }
    // Repinta cada panel SOLO si cambio su dato visible (o si toca redibujo completo). La
    // animacion busy/waiting sigue por su propio camino (animateActivity), asi que esto no la
    // corta: solo evita regenerar ambos sprites en cada snapshot cuando nada cambio.
    // Paneles en "waiting" los dibuja SOLO animateActivity (base + banner); aqui se ignoran. Si
    // toca redibujo completo (salir de overlay/saver, volver online), se invalida su base para
    // que animateActivity la repinte.
    const uint32_t sigC = toolSig(monitorState.claude, claudeSessions, trendDir[0], trendDir[1]);
    if (monitorState.claude.activity == "waiting")
    {
        if (needsFullRedraw)
        {
            waitBase[0] = false;
        }
    }
    // waitBase[0] aun en true aqui = venia mostrando la alerta -> forzar redibujo del panel
    // normal aunque la firma coincida (si no, la pantalla naranja se quedaba pegada).
    else if (needsFullRedraw || waitBase[0] || sigC != prevClaudeSig)
    {
        renderPanel(true);
        prevClaudeSig = sigC;
        waitBase[0] = false;
    }
    const uint32_t sigX = toolSig(monitorState.codex, codexSessions, trendDir[2], trendDir[3]);
    if (monitorState.codex.activity == "waiting")
    {
        if (needsFullRedraw)
        {
            waitBase[1] = false;
        }
    }
    else if (needsFullRedraw || waitBase[1] || sigX != prevCodexSig)
    {
        renderPanel(false);
        prevCodexSig = sigX;
        waitBase[1] = false;
    }
    needsFullRedraw = false;
}

// Base del aviso de pregunta: fondo de acento + icono y nombre FIJOS (no parpadean). Solo el
// "INPUT" y su subtitulo parpadean encima (flashWaitTitle). Se vuelca una vez al entrar.
void drawWaitBase(bool isClaude)
{
    const ToolData &tool = isClaude ? monitorState.claude : monitorState.codex;
    const uint16_t accent = isClaude ? pal.claude : pal.codex;
    const int16_t screenY = isClaude ? 0 : PANEL_H;
    panelSprite.fillSprite(accent);
    drawToolIcon(panelSprite, isClaude, 17, 13, pal.bg);
    String label = effectiveLabel(isClaude, tool);
    label.toUpperCase();
    drawPixelText(panelSprite, 38, 6, label, 3, pal.bg);
    if (!isClaude)
    {
        panelSprite.drawFastHLine(0, 0, SCREEN_W, pal.panelDivider);
    }
    panelSprite.pushSprite(0, screenY);
}

// Parpadeo (aparecer/desaparecer) del "INPUT" + subtitulo en la franja central (volcado chico).
// on: se dibujan (texto oscuro sobre el acento); !on: solo acento (desaparecen).
void flashWaitTitle(bool isClaude, bool on)
{
    if (!ensureSprite(ovlSprite, ovlReady, OVL_W, OVL_H, 4)) // 4bpp: solo 2 colores (acento + fondo)
    {
        return;
    }
    const uint16_t accent = isClaude ? pal.claude : pal.codex;
    const int16_t screenY = isClaude ? 0 : PANEL_H;
    const int16_t bandX = (SCREEN_W - OVL_W) / 2;
    const int16_t bandY = screenY + PANEL_H / 2 - 12;
    // Paleta 4bpp: indice 0 = acento (fondo del aviso), 1 = fondo (texto). Colores EXACTOS.
    uint16_t palOvl[2] = {accent, pal.bg};
    ovlSprite.createPalette(palOvl, 2);
    ovlSprite.fillSprite(0); // acento
    if (on)
    {
        drawPixelTextCenter(ovlSprite, OVL_W / 2, 2, config.waitTitle, 4, 1); // "INPUT" en fondo
        // Subtitulo = solo el mensaje (el nombre ya esta fijo arriba). Escala 2 SIEMPRE: el limite
        // de 34 caracteres garantiza que entra en la banda, sin achicar la fuente.
        String sub = config.waitMsg;
        sub.toUpperCase();
        drawPixelTextCenter(ovlSprite, OVL_W / 2, 30, sub, 2, 1);
    }
    ovlSprite.pushSprite(bandX, bandY);
}

// Anima SOLO la franja de actividad "busy" (no el panel completo): dibuja en stripSprite y la
// vuelca en su sitio. Al ser un area chica, el volcado es ~1-2ms -> sin tearing perceptible.
void animateBusyStrip(bool isClaude)
{
    const ToolData &tool = isClaude ? monitorState.claude : monitorState.codex;
    if (!tool.enabled || tool.activity != "busy")
    {
        return;
    }
    if (!ensureSprite(stripSprite, stripReady, SCREEN_W - 6 - 110, 18, 4)) // 4bpp: fondo + acento
    {
        renderPanel(isClaude); // sin sprite de franja: cae al redibujo del panel completo
        return;
    }
    const int16_t panelTop = isClaude ? 0 : PANEL_H;
    const int16_t boxX = config.animBoxX;
    const int16_t boxY = panelTop + 4;
    const int16_t boxW = (SCREEN_W - 6) - boxX; // <= ancho del sprite (reservado al maximo)
    const uint16_t accent = isClaude ? pal.claude : pal.codex;
    const uint8_t sessions = isClaude ? claudeSessions : codexSessions;
    // Paleta 4bpp: indice 0 = fondo, 1 = acento. drawBusyIndicator solo usa el "acento" -> pasa 1.
    uint16_t palStrip[2] = {pal.bg, accent};
    stripSprite.createPalette(palStrip, 2);
    stripSprite.fillSprite(0); // fondo
    drawBusyIndicator(stripSprite, 0, 0, boxW, 18, 1, sessions); // acento = indice 1
    stripSprite.pushSprite(boxX, boxY);
}

void animateActivity()
{
    if (!monitorState.online || infoOverlayActive)
    {
        return;
    }
    const uint32_t now = millis();

    // Aviso de pregunta (waiting): base invertida estable (un solo volcado al entrar) + banner
    // "INPUT" parpadeando en una franja chica. Asi el flash NO vuelca los 320x120 = sin tearing.
    const bool cWait = monitorState.claude.enabled && monitorState.claude.activity == "waiting";
    const bool xWait = monitorState.codex.enabled && monitorState.codex.activity == "waiting";
    if (cWait && !waitBase[0])
    {
        drawWaitBase(true);
        waitBase[0] = true;
        lastBlinkMs = 0; // fuerza pintar el banner ya
    }
    if (xWait && !waitBase[1])
    {
        drawWaitBase(false);
        waitBase[1] = true;
        lastBlinkMs = 0;
    }
    if (!cWait)
    {
        waitBase[0] = false;
    }
    if (!xWait)
    {
        waitBase[1] = false;
    }
    if (!cWait && !xWait)
    {
        // Al salir de waiting resetea blinkOn: si no, con alertBlinkMs==0 (banner fijo) queda
        // latcheado en true y en la 2a entrada el "if(!blinkOn)" no dibuja el banner -> panel
        // naranja con el nombre pero SIN el "INPUT". Reseteado, la reentrada lo vuelve a pintar.
        blinkOn = false;
    }
    if (cWait || xWait)
    {
        bool doDraw = false;
        if (config.alertBlinkMs == 0)
        {
            if (!blinkOn) // sin flash: banner fijo encendido
            {
                blinkOn = true;
                doDraw = true;
            }
        }
        else if (now - lastBlinkMs >= config.alertBlinkMs)
        {
            lastBlinkMs = now;
            blinkOn = !blinkOn;
            doDraw = true;
        }
        if (doDraw)
        {
            if (cWait)
            {
                flashWaitTitle(true, blinkOn); // solo parpadea el "INPUT" + subtitulo (icono/nombre fijos)
            }
            if (xWait)
            {
                flashWaitTitle(false, blinkOn);
            }
        }
    }

    // Marquee de cuadrados de la actividad busy: re-render del panel al sprite. Cadencia = 1000/animHz
    // (configurable). La posicion es basada en tiempo, asi que el fps solo afecta la suavidad.
    if (now - lastAnimTickMs < (uint32_t)(1000 / config.animHz))
    {
        return;
    }
    lastAnimTickMs = now;
    // Posicion CONTINUA basada en tiempo (no un salto fijo por tick): la velocidad depende de
    // animSpeed y es independiente del framerate, asi se ve fluido aunque varie la cadencia.
    dotScroll = (uint32_t)((uint64_t)now * config.animSpeed / 90);
    // Vuelca solo la franja de actividad (no el panel completo) -> fluido y sin tearing.
    if (monitorState.claude.activity == "busy")
    {
        animateBusyStrip(true);
    }
    if (monitorState.codex.activity == "busy")
    {
        animateBusyStrip(false);
    }
}

// ---------------------------------------------------------------------------
// Sondeo del snapshot (bloqueante, payload pequeno servido desde cache).
// ---------------------------------------------------------------------------

void parseWindow(JsonObjectConst obj, WindowData &window)
{
    window.remaining = obj["remaining_percent"] | 0.0f;
    window.expected = obj["expected_remaining_percent"] | 0.0f;
    window.pace = String((const char *)(obj["pace"] | "unknown"));
    window.resetSeconds = obj["reset_in_seconds"] | 0L;
}

// Solo estos valores dibujan/atenuan bien; cualquier otro (version incompatible, respuesta
// corrupta) dejaria el indicador en blanco y anularia el atenuado anti burn-in.
bool isValidActivity(const String &a)
{
    return a == "idle" || a == "busy" || a == "waiting";
}

void parseTool(JsonObjectConst obj, ToolData &tool)
{
    tool.label = String((const char *)(obj["label"] | ""));
    tool.enabled = obj["enabled"] | true;
    const String act = String((const char *)(obj["activity"] | "idle"));
    tool.activity = isValidActivity(act) ? act : "idle"; // valor desconocido -> idle (no deja la UI en limbo)
    tool.statusText = String((const char *)(obj["status_text"] | ""));
    parseWindow(obj["current"], tool.current);
    parseWindow(obj["weekly"], tool.weekly);
}

// Descarga y parsea el snapshot en la estructura dada. Funcion pura de datos: NO dibuja
// ni toca el estado global (corre en la tarea del core 0). Devuelve si el servicio respondio.
bool fetchSnapshotData(MonitorState &s, const String &baseUrl)
{
    if (baseUrl.length() == 0)
    {
        s.online = false;
        s.lastError = "Configura la URL del servicio";
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(SNAPSHOT_HTTP_TIMEOUT_MS);
    http.setTimeout(SNAPSHOT_HTTP_TIMEOUT_MS);
    if (!http.begin(baseUrl + "/api/esp/snapshot"))
    {
        s.online = false;
        s.lastError = "URL invalida";
        return false;
    }

    const int code = http.GET();
    if (code != 200)
    {
        http.end();
        s.online = false;
        s.lastError = String("HTTP ") + String(code);
        return false;
    }

    // Parseo por streaming (sin materializar el String completo en el heap).
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err)
    {
        s.online = false;
        s.lastError = String("JSON: ") + err.c_str();
        return false;
    }

    // Un 200 con JSON valido pero sin la estructura esperada es un error, no un panel vacio.
    if (!doc["claude"].is<JsonObjectConst>() || !doc["codex"].is<JsonObjectConst>())
    {
        s.online = false;
        s.lastError = "JSON incompleto";
        return false;
    }

    parseTool(doc["claude"], s.claude);
    parseTool(doc["codex"], s.codex);
    s.online = true;
    return true;
}

void fetchSessions(const String &baseUrl)
{
    // Lee el numero de sesiones en paralelo por herramienta desde las cabeceras
    // X-Sessions-* del endpoint /api/esp/frames. Con If-None-Match la respuesta suele
    // ser 304 (solo cabeceras, sin el framebuffer), asi que es una peticion ligera.
    if (baseUrl.length() == 0)
    {
        return;
    }
    HTTPClient http;
    http.setConnectTimeout(SNAPSHOT_HTTP_TIMEOUT_MS);
    http.setTimeout(SNAPSHOT_HTTP_TIMEOUT_MS);
    if (!http.begin(baseUrl + "/api/esp/frames"))
    {
        return;
    }
    const char *headerKeys[] = {"ETag", "X-Sessions-Claude", "X-Sessions-Codex"};
    http.collectHeaders(headerKeys, 3);
    if (framesEtag.length() > 0)
    {
        http.addHeader("If-None-Match", framesEtag);
    }
    const int code = http.GET();
    if (code == 200 || code == 304)
    {
        const String etag = http.header("ETag");
        if (etag.length() > 0)
        {
            framesEtag = etag;
        }
        const String sessionsClaude = http.header("X-Sessions-Claude");
        const String sessionsCodex = http.header("X-Sessions-Codex");
        if (sessionsClaude.length() > 0)
        {
            claudeSessions = (uint8_t)constrain(sessionsClaude.toInt(), 1, 16);
        }
        if (sessionsCodex.length() > 0)
        {
            codexSessions = (uint8_t)constrain(sessionsCodex.toInt(), 1, 16);
        }
        // En 200 (sin If-None-Match, p.ej. primer arranque) llega el framebuffer en el
        // cuerpo; se drena para no dejar bytes en el socket antes de cerrar.
        if (code == 200)
        {
            http.getStream().flush();
            while (http.getStream().available())
            {
                http.getStream().read();
            }
        }
    }
    http.end();
}

// ---------------------------------------------------------------------------
// Long-poll de actividad (bloqueante, corre en la tarea): reacciona casi al instante.
// ---------------------------------------------------------------------------

// Espera un cambio de actividad hasta capMs. El servicio responde de inmediato cuando
// la actividad cambia respecto de ?c=&x=, o retiene hasta su tope. Actualiza s y devuelve
// si hubo cambio. Al agotar capMs sin respuesta simplemente reintenta en la siguiente vuelta.
bool activityLongPoll(MonitorState &s, uint32_t capMs, const String &baseUrl)
{
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(capMs);
    const String url = baseUrl + "/api/esp/activity-wait?c=" +
                       s.claude.activity + "&x=" + s.codex.activity;
    if (!http.begin(url))
    {
        return false;
    }
    const int code = http.GET();
    bool changed = false;
    if (code == 200)
    {
        String body = http.getString();
        body.trim();
        const int space = body.indexOf(' ');
        if (space > 0)
        {
            String nextClaude = body.substring(0, space);
            String nextCodex = body.substring(space + 1);
            nextClaude.trim();
            nextCodex.trim();
            // Solo acepta valores validos {idle,busy,waiting}; ante basura conserva el estado previo.
            if (isValidActivity(nextClaude) && isValidActivity(nextCodex))
            {
                if (nextClaude != s.claude.activity)
                {
                    s.claude.activity = nextClaude;
                    changed = true;
                }
                if (nextCodex != s.codex.activity)
                {
                    s.codex.activity = nextCodex;
                    changed = true;
                }
            }
        }
    }
    http.end();
    return changed;
}

// Publica el estado recien sondeado para que lo consuma el loop (dibujo).
void publishState(const MonitorState &s)
{
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    incomingState = s;
    incomingReady = true;
    xSemaphoreGive(stateMutex);
}

// Tarea de red (core 0): sondea snapshot + sesiones y hace el long-poll de actividad,
// sin tocar nunca la pantalla. El loop (core 1) dibuja a partir del estado publicado.
void pollTask(void *param)
{
    (void)param;
    for (;;)
    {
        pollHeartbeat++; // DIAG
        // Copia atomica de la config que usa este ciclo (evita leer un String que el core1
        // podria estar reasignando). A partir de aqui se trabaja con las copias locales.
        String svc;
        uint32_t pollMs;
        xSemaphoreTake(configMutex, portMAX_DELAY);
        svc = config.serviceUrl;
        pollMs = config.pollIntervalMs;
        xSemaphoreGive(configMutex);

        if (WiFi.status() != WL_CONNECTED || svc.length() == 0)
        {
            // Sin URL configurada: publica un estado offline con aviso accionable (si no, el
            // usuario veria la consola de arranque o el screensaver como si todo estuviera bien).
            if (svc.length() == 0)
            {
                MonitorState s;
                s.online = false;
                s.lastError = "Configura la URL del servicio";
                s.claude.activity = "idle";
                s.codex.activity = "idle";
                publishState(s);
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        const uint32_t cycleStart = millis();
        MonitorState s;
        s.claude.activity = "idle";
        s.codex.activity = "idle";
        statLastPollMs = millis();
        statPollCount++;
        const bool online = fetchSnapshotData(s, svc);
        if (online)
        {
            statLastSuccessMs = millis();
            fetchSessions(svc);
        }
        else
        {
            statFailCount++;
        }
        publishState(s);

        if (online)
        {
            // Espera cambios de actividad (instantaneo) o refresca segun pollIntervalMs
            // (cadencia configurable de sesiones/barras).
            if (activityLongPoll(s, pollMs, svc))
            {
                publishState(s);
            }
            // Piso de cadencia: si el long-poll retorna al instante (servicio que no
            // bloquea), evita el busy-loop que martillearia la red y el core.
            const uint32_t elapsed = millis() - cycleStart;
            if (elapsed < MIN_ONLINE_CYCLE_MS)
            {
                vTaskDelay((MIN_ONLINE_CYCLE_MS - elapsed) / portTICK_PERIOD_MS);
            }
        }
        else
        {
            vTaskDelay(1500 / portTICK_PERIOD_MS);
        }
    }
}

// ---------------------------------------------------------------------------
// Brillo: atenua tras dimAfterSec sin actividad (anti burn-in).
// ---------------------------------------------------------------------------

// Decide el brillo OBJETIVO (rampBrightness lo alcanza gradualmente en el loop).
void updateBrightness()
{
    if (emergencyWifiActive)
    {
        brightnessTargetPct = config.brightnessPct;
        return;
    }
    const bool anyActive = monitorState.online &&
                           (monitorState.claude.activity != "idle" || monitorState.codex.activity != "idle");
    if (config.dimAfterSec == 0 || anyActive)
    {
        brightnessTargetPct = config.brightnessPct;
        return;
    }
    const uint32_t idleMs = millis() - monitorState.lastActivityChangeMs;
    brightnessTargetPct = (idleMs > config.dimAfterSec * 1000UL) ? config.dimBrightnessPct : config.brightnessPct;
}

// ---------------------------------------------------------------------------
// WiFi y punto de acceso de emergencia.
// ---------------------------------------------------------------------------

bool connectWifi()
{
    if (config.wifiSsid.length() == 0)
    {
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(ESP_HOSTNAME);
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

    const uint32_t start = millis();
    drawStartupTitle("CYD USAGE MONITOR"); // titulo una sola vez (anti flicker)
    int32_t lastSec = -1;
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
    {
        const int32_t sec = (millis() - start) / 1000;
        if (sec != lastSec)
        {
            lastSec = sec;
            drawStartupSubtitle("CONECTANDO WIFI " + String(sec) + "S"); // solo la banda del subtitulo
        }
        delay(120);
    }
    return WiFi.status() == WL_CONNECTED;
}

// Espera a que NTP sincronice la hora (hasta timeoutMs) para que los logs tengan fecha real
// desde el arranque, no el epoch. Devuelve si quedo sincronizado.
bool waitForNtp(uint32_t timeoutMs)
{
    const uint32_t start = millis();
    struct tm ti;
    drawStartupTitle("CYD USAGE MONITOR"); // texto constante: se pinta UNA vez fuera del bucle
    drawStartupSubtitle("SINCRONIZANDO HORA");
    while (millis() - start < timeoutMs)
    {
        if (getLocalTime(&ti, 100) && (ti.tm_year + 1900) >= 2024)
        {
            return true;
        }
        delay(150);
    }
    return false;
}

void startEmergencyAp()
{
    emergencyWifiActive = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(EMERGENCY_AP_SSID, EMERGENCY_AP_PASSWORD);
    drawApScreen();
}

// ---------------------------------------------------------------------------
// Servidor web de configuracion.
// ---------------------------------------------------------------------------

String getActiveIp()
{
    if (emergencyWifiActive)
    {
        return WiFi.softAPIP().toString();
    }
    return WiFi.localIP().toString();
}

const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CYD Usage Monitor</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 8 8'><rect width='8' height='8' rx='1.6' fill='%230b1020'/><g fill='%23818cf8'><rect x='1' y='1' width='1' height='1'/><rect x='2' y='1' width='1' height='1'/><rect x='2' y='2' width='1' height='1'/><rect x='3' y='2' width='1' height='1'/><rect x='3' y='3' width='1' height='1'/><rect x='4' y='3' width='1' height='1'/><rect x='2' y='4' width='1' height='1'/><rect x='3' y='4' width='1' height='1'/><rect x='1' y='5' width='1' height='1'/><rect x='2' y='5' width='1' height='1'/><rect x='4' y='6' width='1' height='1'/><rect x='5' y='6' width='1' height='1'/><rect x='6' y='6' width='1' height='1'/><rect x='7' y='6' width='1' height='1'/></g></svg>">
<style>
:root{
 --bg:#f5f6fb;--panel:#fff;--panel2:#fbfcfe;--text:#0f172a;--muted:#64748b;
 --line:#e7e9f0;--soft:#f1f3f9;--accent:#4f46e5;--accent2:#6366f1;--accent-soft:#eef2ff;
 --ok:#16a34a;--ok-soft:#ecfdf5;--warn:#b45309;--warn-soft:#fffbeb;--danger:#dc2626;--danger-soft:#fef2f2;
 --radius:12px;--radius-sm:8px;--shadow:0 1px 2px rgba(15,23,42,.04),0 10px 30px rgba(15,23,42,.06);
}
*{box-sizing:border-box}
body{margin:0;color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;
 background:var(--bg);background-image:radial-gradient(1200px 560px at 100% -10%,#e8ecff 0%,rgba(245,246,251,0) 46%),radial-gradient(900px 500px at -10% 0%,#eafff4 0%,rgba(245,246,251,0) 40%);background-attachment:fixed}
.app{display:grid;grid-template-columns:236px 1fr;min-height:100vh}
aside{border-right:1px solid var(--line);background:rgba(255,255,255,.72);backdrop-filter:blur(8px);padding:20px 14px;position:sticky;top:0;align-self:start;height:100vh}
main{padding:24px 32px 44px;max-width:1120px;width:100%}
.brand{display:flex;align-items:center;gap:12px;margin:0 4px 20px}
.chip{width:36px;height:36px;border-radius:10px;display:flex;align-items:center;justify-content:center;background:var(--accent-soft);color:var(--accent);flex:0 0 auto}
.chip svg,nav button svg{stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;fill:none}
.chip svg{width:19px;height:19px}
.brand .bn{margin:0;font-size:15px;font-weight:700;letter-spacing:-.01em}
.brand p{margin:1px 0 0;font-size:12px;color:var(--muted)}
nav{display:grid;gap:4px}
nav button{display:flex;align-items:center;gap:10px;border:0;width:100%;text-align:left;border-radius:9px;background:transparent;color:var(--muted);padding:11px;font:inherit;font-weight:600;cursor:pointer;transition:background .15s,color .15s}
nav button svg{width:17px;height:17px;flex:0 0 auto}
nav button:hover{background:var(--soft);color:var(--text)}
nav button.active{background:var(--accent-soft);color:var(--accent);font-weight:700}
.phdr{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:18px;flex-wrap:wrap}
.phdr h1{margin:0;font-size:22px;font-weight:700;letter-spacing:-.02em}
.phdr p{margin:4px 0 0;font-size:13px;color:var(--muted)}
.phdr .act{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.page{display:none}
.page.active{display:block;animation:fade .2s ease}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:16px;box-shadow:var(--shadow)}
h2{margin:0 0 14px;font-size:15px;font-weight:700;color:var(--text);padding-left:10px;border-left:3px solid var(--accent);line-height:1.2;letter-spacing:-.01em}
.cards{column-width:320px;column-gap:14px}
.cards>.panel{display:inline-block;width:100%;margin:0 0 14px;break-inside:avoid;-webkit-column-break-inside:avoid}
.stack{display:grid;gap:14px}
.kvs{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media(max-width:560px){.kvs{grid-template-columns:1fr}}
.kv{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:9px 12px;border:1px solid var(--line);border-radius:var(--radius-sm);background:var(--panel2);font-size:13px}
.kv span{color:var(--muted)}.kv b{font-weight:600;color:var(--text);word-break:break-all;text-align:right}
.sysrow{display:flex;justify-content:space-between;align-items:center;font-size:13px;color:var(--muted)}
.sysrow b{color:var(--text);font-weight:600}
.meter{height:8px;border-radius:999px;background:var(--soft);overflow:hidden;margin-top:6px}
.meter i{display:block;height:100%;width:0;background:var(--accent);border-radius:999px;transition:width .4s}
.meter i.warn{background:var(--warn)}.meter i.bad{background:var(--danger)}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 11px;border-radius:999px;border:1px solid var(--line);font-size:12px;font-weight:600;color:var(--muted);background:var(--panel)}
.pill::before{content:"";width:7px;height:7px;border-radius:999px;background:currentColor;flex:0 0 auto}
.pill.ok{color:var(--ok);border-color:#bbf7d0;background:var(--ok-soft)}
.pill.bad{color:var(--danger);border-color:#fecaca;background:var(--danger-soft)}
.badge{display:inline-flex;align-items:center;padding:4px 10px;border-radius:999px;border:1px solid var(--line);font-size:12px;font-weight:600;color:var(--muted);background:var(--panel)}
.btn-sm{display:inline-flex;align-items:center;gap:6px;min-height:32px;padding:0 14px;border:1px solid var(--line);border-radius:var(--radius-sm);background:var(--soft);color:var(--text);font-size:13px;font-weight:600;cursor:pointer;text-decoration:none;transition:background .15s,color .15s,border-color .15s}
.btn-sm:hover{background:var(--accent-soft);color:var(--accent);border-color:transparent}
.btn-sm.danger:hover{background:var(--danger-soft);color:var(--danger)}
.field{margin-top:12px}
.field:first-child{margin-top:0}
.field label{display:block;font-size:13px;font-weight:600;color:var(--text);margin-bottom:4px}
.field small{display:block;margin-top:4px;font-size:12px;color:var(--muted);line-height:1.4}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:520px){.row{grid-template-columns:1fr}}
input,select{width:100%;min-height:40px;border:1px solid var(--line);border-radius:var(--radius-sm);background:var(--panel);color:var(--text);padding:9px 11px;font:inherit;font-size:14px;transition:border-color .15s,box-shadow .15s}
input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
input[type=file]{padding:8px;background:var(--panel2)}
.dropzone{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;padding:32px 20px;border:2px dashed var(--line);border-radius:var(--radius);background:var(--panel2);color:var(--muted);font-size:13px;font-weight:600;text-align:center;cursor:pointer;transition:border-color .15s,background .15s,color .15s}
.dropzone:hover{border-color:var(--accent);color:var(--accent)}
.dropzone.drag{border-color:var(--accent);background:var(--accent-soft);color:var(--accent)}
.dropzone svg{width:30px;height:30px;stroke:currentColor;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
.dropzone b{color:var(--text)}
.dropzone.has b{color:var(--accent)}
input[type=color]{width:56px;height:38px;min-height:0;padding:3px;border-radius:var(--radius-sm);cursor:pointer}
input[type=checkbox]{width:16px;height:16px;min-height:0;padding:0;margin:0;border:0;accent-color:var(--accent);cursor:pointer;flex:0 0 auto}
input::placeholder{color:#94a3b8}
button.primary{width:auto;min-width:220px;padding:0 28px;min-height:44px;border:0;border-radius:var(--radius-sm);background:var(--accent);color:#fff;font-weight:700;font-size:15px;cursor:pointer;box-shadow:0 6px 16px rgba(79,70,229,.25);transition:filter .15s,transform .1s}
button.primary:hover{filter:brightness(1.06)}button.primary:active{transform:translateY(1px)}
button.primary:disabled{opacity:.6;cursor:default;filter:none}
button.ghost{margin-top:0;width:auto;min-height:40px;padding:0 18px;border:1px solid var(--line);border-radius:var(--radius-sm);background:var(--soft);color:var(--text);font:inherit;font-weight:600;cursor:pointer;transition:background .15s,color .15s,border-color .15s}
button.ghost:hover{background:var(--accent-soft);color:var(--accent);border-color:transparent}
button.ghost.block{width:100%;margin-top:12px}
button.ghost.sm{min-height:34px;padding:0 14px;font-size:13px}
button:focus-visible,a.btn-sm:focus-visible,.lvl input:focus-visible,input[type=color]:focus-visible{outline:none;box-shadow:0 0 0 3px var(--accent-soft);border-color:var(--accent)}
nav button:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
small{color:var(--muted)}
.logwin{background:#0b1020;color:#d7def0;border-radius:var(--radius-sm);padding:14px;margin:0;font:13px/1.55 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all;max-height:64vh;min-height:300px;overflow:auto}
.logbar{display:flex;flex-wrap:wrap;gap:12px 24px;align-items:flex-end;margin-bottom:14px}
.lvls{display:flex;gap:16px;flex-wrap:wrap;flex-shrink:0}
.lvl{display:inline-flex;align-items:center;gap:6px;font-size:13px;font-weight:600;color:var(--text);cursor:pointer;user-select:none}
.dates{display:flex;gap:10px;flex-wrap:wrap;align-items:flex-end;font-size:12px;color:var(--muted)}
.dates label{display:flex;flex-direction:column;gap:3px}
.dates input{width:auto}
.lg{display:block}
.lg-DEBUG{color:#8a93a6}.lg-INFO{color:#cdd6ea}.lg-WARN{color:#e6b23c}.lg-ERROR{color:#ef6b6b;font-weight:600}
#toast{position:fixed;bottom:20px;right:20px;z-index:60;display:flex;flex-direction:column;gap:8px;max-width:340px}
#toast .t{padding:11px 14px;border-radius:var(--radius-sm);font-size:13px;font-weight:600;box-shadow:var(--shadow);background:var(--panel);border:1px solid var(--line);color:var(--text);animation:fade .2s ease}
#toast .t.ok{background:var(--ok-soft);border-color:#bbf7d0;color:var(--ok)}
#toast .t.bad{background:var(--danger-soft);border-color:#fecaca;color:var(--danger)}
@media(max-width:640px){
 .app{grid-template-columns:1fr}
 aside{position:static;height:auto;border-right:0;border-bottom:1px solid var(--line)}
 nav{display:flex;overflow-x:auto;gap:6px}
 nav button{flex:0 0 auto;min-height:44px}
 main{padding:18px 16px 40px}
 button.primary{width:100%;min-width:0}
 .dates input,.lvl{min-height:40px}
}
</style></head><body>
<div class="app">
<aside>
 <div class="brand">
  <div class="chip"><svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 20h8M12 17v3M6 11l2.5-3 2 4L13 8l2 3"/></svg></div>
  <div><p class="bn">CYD Usage Monitor</p><p id="brandsub">cyd.local</p></div>
 </div>
 <nav aria-label="Secciones">
  <button data-page="estado" class="active" aria-current="page"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 12h4l2 6 4-14 2 8h6"/></svg>Estado</button>
  <button data-page="config"><svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M12 3v3M12 18v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M3 12h3M18 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1"/></svg>Config</button>
  <button data-page="logs"><svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/></svg>Logs</button>
  <button data-page="files"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 7l1.5-2h5L11 7h10v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/><path d="M12 11v6M9 14l3 3 3-3"/></svg>Filesystem</button>
  <button data-page="ota"><svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="5" width="14" height="14" rx="1"/><path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2M10 10h4v4h-4z"/></svg>Firmware</button>
 </nav>
</aside>
<main>

<section id="page-estado" class="page active">
 <div class="phdr"><div><h1>Estado</h1><p>Estado actual del dispositivo</p></div><div class="act"><span id="live" class="pill">...</span></div></div>
 <div class="cards">
  <div class="panel"><h2>Dispositivo</h2><div class="kvs" id="status" style="grid-template-columns:1fr"></div></div>
  <div class="panel"><h2>Sistema</h2>
   <div class="sysrow"><span>RAM (heap)</span><b id="sysram">-</b></div>
   <div class="meter"><i id="rambar"></i></div>
   <div class="sysrow" style="margin-top:12px"><span>Almacenamiento (config)</span><b id="sysfs">-</b></div>
   <div class="meter"><i id="fsbar"></i></div>
   <div class="kvs" id="sysmore" style="grid-template-columns:1fr 1fr;margin-top:14px"></div>
  </div>
 </div>
</section>

<section id="page-config" class="page">
 <div class="phdr"><div><h1>Config</h1><p>Ajustes del CYD, se aplican al reiniciar</p></div><div class="act"><button class="primary" form="cfg" type="submit">Guardar y reiniciar</button></div></div>
 <form id="cfg">
  <div class="cards">
   <div class="panel"><h2>Red</h2>
    <div class="field"><label for="ssid">WiFi SSID</label><input name="ssid" id="ssid"></div>
    <div class="field"><label for="pass">WiFi clave</label><input name="pass" id="pass" type="password" placeholder="(sin cambios)"></div>
   </div>
   <div class="panel"><h2>Servicio</h2>
    <div class="field"><label for="service">URL del servidor de datos</label><input name="service" id="service" placeholder="http://192.168.1.100:8765"><small>Ejemplo: http://IP-DEL-PC:8765</small></div>
    <div class="field"><label for="poll">Frecuencia de actualización (ms)</label><input name="poll" id="poll" type="number" min="1000" step="500"><small>Mínimo 1000 ms (1 s).</small></div>
   </div>
   <div class="panel"><h2>Pantalla</h2>
    <div class="row">
     <div class="field"><label for="rot">Rotación</label><select name="rot" id="rot"><option value="1">Normal</option><option value="3">Invertida (180)</option></select></div>
     <div class="field"><label for="inv">Inversión de color</label><select name="inv" id="inv"><option value="1">Activada</option><option value="0">Desactivada</option></select></div>
    </div>
    <small style="display:block;margin-top:6px">Deja la inversión Activada si los colores se ven invertidos.</small>
    <div class="row" style="margin-top:12px">
     <div class="field"><label for="bright">Brillo (%)</label><input name="bright" id="bright" type="number" min="5" max="100"></div>
     <div class="field"><label for="dimb">Brillo atenuado (%)</label><input name="dimb" id="dimb" type="number" min="0" max="100"></div>
    </div>
    <div class="field"><label for="dima">Atenuar tras (segundos)</label><input name="dima" id="dima" type="number" min="0"><small>Sin actividad por ese tiempo, baja al brillo atenuado. 0 = nunca.</small></div>
   </div>
   <div class="panel"><h2>Animación</h2>
    <div class="row">
     <div class="field"><label for="aspd">Velocidad (1-15)</label><input name="aspd" id="aspd" type="number" min="1" max="15"></div>
     <div class="field"><label for="abx">Ancho del área</label><input name="abx" id="abx" type="number" min="110" max="280"><small>Borde izq. (110-280). Menor = más ancho.</small></div>
    </div>
    <div class="field"><label for="asq">Tamaño de los cuadros (4-14)</label><input name="asq" id="asq" type="number" min="4" max="14"><small>Tamaño en píxeles de los cuadros de la animación de actividad.</small></div>
    <div class="field"><label for="asty">Estilo de la animación (ocupado)</label><select name="asty" id="asty"><option value="0">Cuadros (marquee)</option><option value="1">Pulso</option><option value="2">Barra</option></select></div>
    <div class="field"><label for="ahz">Fluidez de la animación</label><select name="ahz" id="ahz"><option value="15">15 Hz</option><option value="20">20 Hz</option><option value="25">25 Hz</option><option value="30">30 Hz</option><option value="40">40 Hz</option><option value="50">50 Hz</option><option value="60">60 Hz</option></select><small>Cuadros por segundo (10-60). Más alto = más suave; no cambia la velocidad.</small></div>
   </div>
   <div class="panel"><h2>Tendencia del superávit</h2>
    <div class="row">
     <div class="field"><label for="twin">Ventana (minutos)</label><input name="twin" id="twin" type="number" min="1" max="1440"></div>
     <div class="field"><label for="tmar">Margen (%)</label><input name="tmar" id="tmar" type="number" min="0" max="50"></div>
    </div>
    <small style="display:block;margin-top:8px">La flecha junto al % de cada ventana indica si el superávit subió, bajó o se mantuvo (dentro del margen) en ese lapso. Punto = estable.</small>
    <div class="field" style="margin-top:12px"><label for="alrt">Alerta si queda ≤ (%)</label><input name="alrt" id="alrt" type="number" min="0" max="100"><small>Cuando el % restante baja de este umbral, el número y la barra se ponen en rojo. 0 = desactivado.</small></div>
   </div>
   <div class="panel"><h2>Colores</h2>
    <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px">
     <button type="button" class="ghost sm" data-theme="dark">Oscuro</button>
     <button type="button" class="ghost sm" data-theme="light">Claro</button>
     <button type="button" class="ghost sm" data-theme="contrast">Alto contraste</button>
    </div>
    <div class="row">
     <div class="field"><label for="ccol">Acento Claude</label><input name="ccol" id="ccol" type="color"></div>
     <div class="field"><label for="xcol">Acento Codex</label><input name="xcol" id="xcol" type="color"></div>
    </div>
    <div class="row" style="margin-top:12px">
     <div class="field"><label for="tcol">Texto principal</label><input name="tcol" id="tcol" type="color"></div>
     <div class="field"><label for="mcol">Texto secundario</label><input name="mcol" id="mcol" type="color"></div>
    </div>
    <div class="row" style="margin-top:12px">
     <div class="field"><label for="bgc">Fondo</label><input name="bgc" id="bgc" type="color"></div>
     <div class="field"><label for="dsc">Deshabilitado</label><input name="dsc" id="dsc" type="color"></div>
    </div>
    <div class="row" style="margin-top:12px">
     <div class="field"><label for="gdc">Positivo</label><input name="gdc" id="gdc" type="color"></div>
     <div class="field"><label for="wnc">Advertencia</label><input name="wnc" id="wnc" type="color"></div>
    </div>
    <div class="row" style="margin-top:12px">
     <div class="field"><label for="bdc">Negativo</label><input name="bdc" id="bdc" type="color"></div>
     <div class="field"><label for="dvc">Divisor</label><input name="dvc" id="dvc" type="color"></div>
    </div>
    <div class="field" style="margin-top:12px"><label for="bbc">Fondo de barra</label><input name="bbc" id="bbc" type="color" style="width:56px"></div>
    <small style="display:block;margin-top:8px">Acentos: % y barra de cada agente. Texto principal: números del reset. Texto secundario: 5H/WK y unidades H/D/M. Positivo: tendencia arriba y estados OK. Negativo: déficit, tendencia abajo y errores.</small>
   </div>
   <div class="panel"><h2>Screensaver</h2>
    <div class="field"><label for="sav">Efecto (cuando no hay servicio)</label><select name="sav" id="sav"><option value="0">Ninguno (mostrar "Sin servicio")</option><option value="1">Matrix</option><option value="2">DVD</option><option value="3">Estrellas</option><option value="4">Snake</option></select></div>
    <button type="button" class="ghost block" id="savTest">Probar en la pantalla</button>
   </div>
   <div class="panel"><h2>Alerta de pregunta</h2>
    <div class="field"><label for="wt">Título (máx. 10)</label><input name="wt" id="wt" maxlength="10"></div>
    <div class="field"><label for="wm">Mensaje (máx. 34)</label><input name="wm" id="wm" maxlength="34"><small>Se antepone el nombre del agente.</small></div>
    <div class="field"><label for="ablk">Parpadeo (ms)</label><input name="ablk" id="ablk" type="number" min="0" max="5000" step="50"><small>Cada cuánto parpadea el panel al preguntar. 0 = fijo (sin parpadeo).</small></div>
   </div>
   <div class="panel"><h2>Alias de agente</h2>
    <div class="row">
     <div class="field"><label for="clbl">Alias Claude</label><input name="clbl" id="clbl" maxlength="12"></div>
     <div class="field"><label for="xlbl">Alias Codex</label><input name="xlbl" id="xlbl" maxlength="12"></div>
    </div>
    <small style="display:block;margin-top:8px">Nombre a mostrar en pantalla. Vacío = usar el que envía el servicio.</small>
   </div>
   <div class="panel"><h2>Seguridad</h2>
    <div class="field"><label for="wpass">Clave web (usuario admin)</label><input name="wpass" id="wpass" type="password" placeholder="(sin cambios)"><small>Vacía = web sin clave.</small></div>
   </div>
   <div class="panel"><h2>Copias en SD</h2>
    <button type="button" class="ghost block" id="bkSave">Guardar copia de la configuración</button>
    <div class="kvs" id="bkList" style="grid-template-columns:1fr;margin-top:12px"></div>
   </div>
  </div>
 </form>
</section>

<section id="page-ota" class="page">
 <div class="phdr"><div><h1>Firmware</h1><p>Actualización OTA del firmware del dispositivo</p></div></div>
 <div class="panel" style="max-width:640px">
  <form id="otaForm" method="POST" action="/update" enctype="multipart/form-data">
   <label for="fw" id="drop" class="dropzone">
    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 15V4M8 8l4-4 4 4"/><path d="M4 15v4a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-4"/></svg>
    <span id="dropText">Arrastra el archivo <b>.bin</b> aquí o haz clic para elegir</span>
   </label>
   <input type="file" id="fw" name="firmware" accept=".bin" hidden>
   <button class="primary" style="width:100%" type="submit">Subir firmware</button>
  </form>
  <p style="margin:12px 0 0"><small id="ota"></small></p>
 </div>
</section>

<section id="page-logs" class="page">
 <div class="phdr"><div><h1>Logs</h1><p>Eventos del dispositivo en tiempo real</p></div><div class="act"><span class="pill ok" id="loglive">En vivo</span><span class="badge" id="logboot">arranque</span><span class="badge" id="logmeta">0 B</span></div></div>
 <div class="panel">
  <div class="logbar">
   <div class="lvls">
    <label class="lvl"><input type="checkbox" class="lvlchk" value="DEBUG"> DEBUG</label>
    <label class="lvl"><input type="checkbox" class="lvlchk" value="INFO" checked> INFO</label>
    <label class="lvl"><input type="checkbox" class="lvlchk" value="WARN" checked> WARN</label>
    <label class="lvl"><input type="checkbox" class="lvlchk" value="ERROR" checked> ERROR</label>
   </div>
   <div class="dates">
    <label for="logFrom">Desde<input type="datetime-local" id="logFrom"></label>
    <label for="logTo">Hasta<input type="datetime-local" id="logTo"></label>
    <button type="button" class="ghost sm" id="logPause">Pausar</button><button type="button" class="ghost sm" id="logClear">Limpiar</button>
   </div>
  </div>
  <pre class="logwin" id="logs">cargando...</pre>
 </div>
</section>

<section id="page-files" class="page">
 <div class="phdr"><div><h1>Filesystem</h1><p>Archivos de log en la SD (más reciente arriba)</p></div><div class="act"><button type="button" class="ghost sm" id="fileRefresh">Refrescar</button></div></div>
 <div class="panel" style="max-width:720px"><div class="kvs" id="fileList" style="grid-template-columns:1fr">cargando...</div></div>
</section>

</main>
</div>
<div id="toast" role="status" aria-live="polite"></div>
<script>
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function kv(k,v){return '<div class="kv"><span>'+k+'</span><b>'+esc(v)+'</b></div>'}
function toast(msg,type){const w=document.getElementById('toast');const t=document.createElement('div');t.className='t '+(type||'');t.textContent=msg;w.appendChild(t);setTimeout(()=>t.remove(),4000);}
document.querySelectorAll('nav button').forEach(b=>b.addEventListener('click',()=>{
 document.querySelectorAll('nav button').forEach(x=>{x.classList.remove('active');x.removeAttribute('aria-current');});
 document.querySelectorAll('.page').forEach(x=>x.classList.remove('active'));
 b.classList.add('active');b.setAttribute('aria-current','page');
 document.getElementById('page-'+b.dataset.page).classList.add('active');
 if(b.dataset.page=='files')loadFiles();
}));
function paintStatus(s){
  const live=document.getElementById('live');
  live.textContent=s.online?'En línea':'Sin conexión';live.className='pill '+(s.online?'ok':'bad');
  document.getElementById('status').innerHTML=kv('IP',s.ip)+kv('WiFi',s.wifi)+kv('mDNS',s.mdns)+kv('Tarjeta SD',s.sd)+kv('Almacenamiento interno',s.fs)+kv('Arranques','#'+s.bootn)+kv('ID de sesión',s.boot);
  const lb=document.getElementById('logboot');if(lb)lb.textContent='#'+s.bootn+' '+s.boot;
  ota.textContent=s.ota;
  const sub=document.getElementById('brandsub');if(sub&&s.ip)sub.textContent=s.ip;
  // Panel Sistema
  if(s.heapTotal){
    const ramUsed=s.heapTotal-s.heapFree, ramPct=Math.round(ramUsed/s.heapTotal*100);
    sysram.textContent=(s.heapFree/1024).toFixed(0)+' KB libres / '+(s.heapTotal/1024).toFixed(0)+' KB';
    setBar('rambar',ramPct);
    const fsPct=s.fsTotal?Math.round(s.fsUsed/s.fsTotal*100):0;
    sysfs.textContent=(s.fsUsed/1024).toFixed(1)+' / '+(s.fsTotal/1024).toFixed(0)+' KB ('+fsPct+'%)';
    setBar('fsbar',fsPct);
    document.getElementById('sysmore').innerHTML=
      kv('CPU (loop)',s.loopHz+' Hz')+kv('Señal WiFi',s.rssi+' dBm')+
      kv('Tarjeta SD',s.sdTotal?(s.sdTotal/1048576).toFixed(1)+' GB':'no')+kv('Heap mín.',(s.heapMin/1024).toFixed(0)+' KB')+
      kv('Sondeos',s.polls+' ('+s.fails+' fallos)')+kv('Uptime',fmtUptime(s.uptime));
  }
}
function setBar(id,pct){const e=document.getElementById(id);if(!e)return;e.style.width=Math.min(100,pct)+'%';e.className=pct>=95?'bad':pct>=88?'warn':'';}
function fmtUptime(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m';}
async function pollStatus(){try{paintStatus(await (await fetch('/api/status',{cache:'no-store'})).json());}catch(e){}}
async function load(){
 try{
  paintStatus(await (await fetch('/api/status')).json());
  const c=await (await fetch('/api/config')).json();
  ssid.value=c.ssid;service.value=c.service;poll.value=c.poll;rot.value=c.rot;
  inv.value=c.inv?'1':'0';bright.value=c.bright;dimb.value=c.dimb;dima.value=c.dima;
  aspd.value=c.aspd;abx.value=c.abx;wt.value=c.wt;wm.value=c.wm;sav.value=c.sav;
  asq.value=c.asq;twin.value=c.twin;tmar.value=c.tmar;alrt.value=c.alrt;
  asty.value=c.asty;ahz.value=c.ahz;ablk.value=c.ablk;clbl.value=c.clbl||'';xlbl.value=c.xlbl||'';
  ccol.value='#'+c.ccol;xcol.value='#'+c.xcol;tcol.value='#'+c.tcol;mcol.value='#'+c.mcol;
  bgc.value='#'+c.bgc;dsc.value='#'+c.dsc;gdc.value='#'+c.gdc;wnc.value='#'+c.wnc;bdc.value='#'+c.bdc;dvc.value='#'+c.dvc;bbc.value='#'+c.bbc;
  wpass.placeholder=c.wpassSet?'(sin cambios)':'(sin clave)';
 }catch(e){toast('No se pudo cargar la configuración','bad');}
}
document.getElementById('cfg').addEventListener('submit',async e=>{
 e.preventDefault();
 if(!confirm('Guardar la configuración y reiniciar el dispositivo ahora?'))return;
 const btn=e.submitter;if(btn){btn.disabled=true;btn.textContent='Guardando...';}
 const f=new FormData(e.target);const b=new URLSearchParams();for(const[k,v]of f)b.append(k,v);
 try{const r=await fetch('/api/config',{method:'POST',body:b});
  if(r.ok)toast('Guardado. El dispositivo se está reiniciando...','ok');else toast('Error al guardar ('+r.status+')','bad');
 }catch(e){toast('Error de red al guardar','bad');}
 if(btn){btn.disabled=false;btn.textContent='Guardar y reiniciar';}
});
document.getElementById('otaForm').addEventListener('submit',e=>{
 e.preventDefault();
 const file=e.target.firmware.files[0];
 if(!file){toast('Selecciona un archivo .bin','bad');return;}
 if(!confirm('Subir firmware y reiniciar? No apagues el equipo durante la actualización.'))return;
 const btn=e.target.querySelector('button');btn.disabled=true;
 const xhr=new XMLHttpRequest();xhr.open('POST','/update');
 xhr.upload.onprogress=ev=>{if(ev.lengthComputable)ota.textContent='Subiendo '+Math.round(ev.loaded/ev.total*100)+'%';};
 xhr.onload=()=>{const ok=xhr.status<300;ota.textContent=ok?'Firmware subido, reiniciando...':'Error '+xhr.status;toast(ok?'Firmware subido':'Error al subir firmware',ok?'ok':'bad');btn.disabled=false;};
 xhr.onerror=()=>{ota.textContent='Error de red';toast('Error de red al subir','bad');btn.disabled=false;};
 xhr.send(new FormData(e.target));
});
(function(){
 const drop=document.getElementById('drop'), fw=document.getElementById('fw'), dt=document.getElementById('dropText');
 function show(){if(fw.files[0]){dt.innerHTML='<b>'+esc(fw.files[0].name)+'</b> ('+(fw.files[0].size/1024).toFixed(0)+' KB)';drop.classList.add('has');}else{drop.classList.remove('has');}}
 fw.addEventListener('change',show);
 ['dragenter','dragover'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add('drag');}));
 ['dragleave','dragend','drop'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove('drag');}));
 drop.addEventListener('drop',e=>{if(e.dataTransfer.files.length){fw.files=e.dataTransfer.files;show();}});
})();
let rawLogs='';
function renderLogs(){
 const active=new Set([...document.querySelectorAll('.lvlchk:checked')].map(c=>c.value));
 const from=document.getElementById('logFrom').value, to=document.getElementById('logTo').value;
 const el=document.getElementById('logs');
 const atBottom=el.scrollHeight-el.scrollTop-el.clientHeight<24;
 const html=rawLogs.split('\n').filter(Boolean).map(l=>{
  const m=l.match(/^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[([A-Z0-9]{3}-[A-Z0-9]{3})\] \[(DEBUG|INFO|WARN|ERROR)\] ([\s\S]*)$/);
  if(!m) return '<span class="lg lg-INFO">'+esc(l)+'</span>';
  if(!active.has(m[3])) return '';
  const iso=m[1].replace(' ','T');
  if(from && iso<from) return '';
  if(to && iso>to+':59') return '';
  return '<span class="lg lg-'+m[3]+'">'+esc(l)+'</span>';
 }).filter(Boolean).join('');
 el.innerHTML=html||'<span class="lg lg-INFO">(sin líneas para el filtro)</span>';
 if(atBottom)el.scrollTop=el.scrollHeight;
}
async function loadLogs(){
 try{
  const d=await (await fetch('/api/logs',{cache:'no-store'})).json();
  rawLogs=d.logs||'';document.getElementById('logmeta').textContent='Buffer '+((d.bytes||0)/1024).toFixed(1)+' KB';renderLogs();
 }catch(e){}
}
document.querySelectorAll('.lvlchk,#logFrom,#logTo').forEach(x=>x.addEventListener('input',renderLogs));
document.getElementById('logClear').onclick=()=>{document.getElementById('logFrom').value='';document.getElementById('logTo').value='';renderLogs();};
async function loadBackups(){
 try{
  const arr=await (await fetch('/api/backups',{cache:'no-store'})).json();
  const el=document.getElementById('bkList');
  if(!arr.length){el.innerHTML='<div class="kv"><span>Sin copias en la SD</span></div>';return;}
  el.innerHTML=arr.map(b=>'<div class="kv"><span>'+esc(b.name)+'</span><button type="button" class="btn-sm danger" data-f="'+esc(b.name)+'">Restaurar</button></div>').join('');
  el.querySelectorAll('button[data-f]').forEach(x=>x.onclick=async()=>{
   if(!confirm('Restaurar '+x.dataset.f+' y reiniciar?'))return;
   try{const r=await fetch('/api/restore?file='+encodeURIComponent(x.dataset.f),{method:'POST'});
    if(r.ok)toast('Restaurando, reiniciando...','ok');else toast('Error al restaurar','bad');
   }catch(e){toast('Error de red','bad');}
  });
 }catch(e){document.getElementById('bkList').innerHTML='<div class="kv"><span>No se pudo cargar</span><button class="btn-sm" onclick="loadBackups()">Reintentar</button></div>';}
}
document.getElementById('bkSave').onclick=async(e)=>{
 const b=e.target;b.disabled=true;b.textContent='Guardando...';
 try{const r=await fetch('/api/backup',{method:'POST'});const d=await r.json().catch(()=>({}));
  if(d.ok){toast('Copia guardada: '+d.file,'ok');loadBackups();}else toast('Error: '+(d.error||'sin SD'),'bad');
 }catch(e){toast('Error de red','bad');}
 b.disabled=false;b.textContent='Guardar copia de la configuración';
};
async function loadFiles(){
 try{
  const arr=await (await fetch('/api/files',{cache:'no-store'})).json();
  const el=document.getElementById('fileList');
  if(!arr.length){el.innerHTML='<div class="kv"><span>Sin archivos (SD no detectada o sin logs aún)</span></div>';return;}
  el.innerHTML=arr.map(f=>'<div class="kv"><span>'+esc(f.name)+' <span style="color:var(--muted)">('+(f.size/1024).toFixed(1)+' KB)</span></span><a class="btn-sm" href="/api/download?f='+encodeURIComponent(f.name)+'" download>Descargar</a></div>').join('');
 }catch(e){document.getElementById('fileList').innerHTML='<div class="kv"><span>No se pudo cargar</span><button class="btn-sm" onclick="loadFiles()">Reintentar</button></div>';}
}
document.getElementById('fileRefresh').onclick=loadFiles;
document.getElementById('savTest').onclick=async()=>{
 const id=document.getElementById('sav').value;
 if(id=='0'){toast('Selecciona un efecto (no Ninguno) para probarlo','bad');return;}
 try{await fetch('/api/saver?id='+id,{method:'POST'});toast('Efecto enviado al CYD','ok');}catch(e){toast('Error de red','bad');}
};
const THEMES={
 dark:{bgc:'#000000',tcol:'#ebedf0',mcol:'#8c929c',dsc:'#606670',dvc:'#30343c',bbc:'#262930',gdc:'#46c878',wnc:'#e8aa2d',bdc:'#e05046'},
 light:{bgc:'#f2f4f8',tcol:'#0f172a',mcol:'#5b6472',dsc:'#94a3b8',dvc:'#c8ccd6',bbc:'#dbe0ea',gdc:'#1f9d57',wnc:'#b45309',bdc:'#c0362c'},
 contrast:{bgc:'#000000',tcol:'#ffffff',mcol:'#c8ccd6',dsc:'#888888',dvc:'#ffffff',bbc:'#333333',gdc:'#00e676',wnc:'#ffd400',bdc:'#ff3b30'}
};
document.querySelectorAll('[data-theme]').forEach(b=>b.onclick=()=>{const t=THEMES[b.dataset.theme];for(const k in t){const e=document.getElementById(k);if(e)e.value=t[k];}toast('Tema aplicado (revisa y Guarda)','ok');});
load();loadLogs();loadBackups();loadFiles();setInterval(pollStatus,5000);
let logTimer=setInterval(loadLogs,2000);
document.getElementById('logPause').onclick=e=>{if(logTimer){clearInterval(logTimer);logTimer=null;e.target.textContent='Reanudar';const l=document.getElementById('loglive');l.textContent='Pausado';l.className='pill';}else{logTimer=setInterval(loadLogs,2000);loadLogs();e.target.textContent='Pausar';const l=document.getElementById('loglive');l.textContent='En vivo';l.className='pill ok';}};
</script>
</body></html>)HTML";

void handleRoot()
{
    // no-store: evita que el navegador cachee una version vieja de la pagina tras flashear.
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", PAGE_HTML);
}

void handleGetConfig()
{
    String json = "{";
    json += "\"ssid\":\"" + jsonEscape(config.wifiSsid) + "\",";
    json += "\"service\":\"" + jsonEscape(config.serviceUrl) + "\",";
    json += "\"poll\":" + String(config.pollIntervalMs) + ",";
    json += "\"rot\":" + String(config.rotation) + ",";
    json += "\"bright\":" + String(config.brightnessPct) + ",";
    json += "\"dimb\":" + String(config.dimBrightnessPct) + ",";
    json += "\"dima\":" + String(config.dimAfterSec) + ",";
    json += "\"inv\":" + String(config.invertDisplay ? "true" : "false") + ",";
    json += "\"aspd\":" + String(config.animSpeed) + ",";
    json += "\"abx\":" + String(config.animBoxX) + ",";
    json += "\"wt\":\"" + jsonEscape(config.waitTitle) + "\",";
    json += "\"wm\":\"" + jsonEscape(config.waitMsg) + "\",";
    // Nunca se envia la clave real; solo si esta puesta (para el placeholder de la UI).
    json += "\"wpassSet\":" + String(config.webPassword.length() > 0 ? "true" : "false") + ",";
    json += "\"sav\":" + String(config.screensaver) + ",";
    json += "\"ccol\":\"" + config.claudeColor + "\",";
    json += "\"xcol\":\"" + config.codexColor + "\",";
    json += "\"tcol\":\"" + config.textColor + "\",";
    json += "\"mcol\":\"" + config.mutedColor + "\",";
    json += "\"twin\":" + String(config.trendWindowMin) + ",";
    json += "\"tmar\":" + String(config.trendMarginPct) + ",";
    json += "\"asq\":" + String(config.animSquarePx) + ",";
    json += "\"bgc\":\"" + config.bgColor + "\",";
    json += "\"dvc\":\"" + config.dividerColor + "\",";
    json += "\"bbc\":\"" + config.barBgColor + "\",";
    json += "\"gdc\":\"" + config.goodColor + "\",";
    json += "\"wnc\":\"" + config.warnColor + "\",";
    json += "\"bdc\":\"" + config.badColor + "\",";
    json += "\"dsc\":\"" + config.disabledColor + "\",";
    json += "\"alrt\":" + String(config.alertThresholdPct) + ",";
    json += "\"clbl\":\"" + jsonEscape(config.claudeLabel) + "\",";
    json += "\"xlbl\":\"" + jsonEscape(config.codexLabel) + "\",";
    json += "\"asty\":" + String(config.animStyle) + ",";
    json += "\"ahz\":" + String(config.animHz) + ",";
    json += "\"ablk\":" + String(config.alertBlinkMs) + "";
    json += "}";
    server.send(200, "application/json", json);
}

// Auth basica para operaciones sensibles (config y OTA web). Solo se exige si hay clave
// web definida; vacia = web abierta (LAN de confianza). Credenciales: WEB_USER + webPassword.
bool requireAuth()
{
    if (config.webPassword.length() == 0)
    {
        return true;
    }
    if (!server.authenticate(WEB_USER, config.webPassword.c_str()))
    {
        server.requestAuthentication();
        return false;
    }
    return true;
}

void handleGetStatus()
{
    // El SSID (WiFi.SSID()) puede traer comillas/backslash: se escapa para no romper el JSON.
    const String wifiName = emergencyWifiActive ? String("AP: ") + EMERGENCY_AP_SSID : getActiveWifiSsidSafe();
    String json = "{";
    json += "\"ip\":\"" + jsonEscape(getActiveIp()) + "\",";
    json += "\"wifi\":\"" + jsonEscape(wifiName) + "\",";
    json += "\"online\":" + String(monitorState.online ? "true" : "false") + ",";
    json += "\"mdns\":\"" + String(LOCAL_DOMAIN) + "\",";
    json += "\"sd\":\"" + String(sdReady ? String((uint32_t)(SD.cardSize() / (1024 * 1024))) + " MB (total)" : "no detectada") + "\",";
    json += "\"fs\":\"" + String(littleFsReady ? String((uint32_t)(LittleFS.totalBytes() / 1024)) + " KB (config)" : "no") + "\",";
    json += "\"boot\":\"" + bootId + "\",";
    json += "\"bootn\":" + String(bootCount) + ",";
    json += "\"ota\":\"" + jsonEscape(webOtaStatus) + "\",";
    // Estado de sistema (pagina Estado). Todo barato: sin escaneos lentos (no ralentiza el resto).
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"heapMin\":" + String(ESP.getMinFreeHeap()) + ",";
    json += "\"heapTotal\":" + String(ESP.getHeapSize()) + ",";
    json += "\"fsUsed\":" + String(littleFsReady ? (uint32_t)LittleFS.usedBytes() : 0) + ",";
    json += "\"fsTotal\":" + String(littleFsReady ? (uint32_t)LittleFS.totalBytes() : 0) + ",";
    json += "\"sdTotal\":" + String(sdReady ? (uint32_t)(SD.cardSize() / 1024) : 0) + ",";
    json += "\"loopHz\":" + String(loopHz) + ",";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"polls\":" + String(statPollCount) + ",";
    json += "\"fails\":" + String(statFailCount) + ",";
    json += "\"err\":\"" + jsonEscape(monitorState.lastError) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleGetLogs()
{
    String json = "{\"logs\":\"" + jsonEscape(logBuffer) + "\",\"bytes\":" + String(logBuffer.length()) + "}";
    server.send(200, "application/json", json);
}

// Solo el nombre de archivo (sin ruta) para evitar path traversal en la restauracion.
String backupBasename(const String &name)
{
    const int slash = name.lastIndexOf('/');
    return slash >= 0 ? name.substring(slash + 1) : name;
}

// Guarda la config actual como /backups/cfg-<n>.json (n autoincremental). Devuelve la ruta.
bool saveConfigBackup(String &pathOut)
{
    if (!sdReady)
    {
        return false;
    }
    SdGuard _sd; // serializa el acceso a SD contra la tarea logger (core0)
    if (!SD.exists(BACKUP_DIR))
    {
        SD.mkdir(BACKUP_DIR);
    }
    int n = 1;
    while (SD.exists(String(BACKUP_DIR) + "/cfg-" + String(n) + ".json"))
    {
        n++;
    }
    pathOut = String(BACKUP_DIR) + "/cfg-" + String(n) + ".json";
    File f = SD.open(pathOut, FILE_WRITE);
    if (!f)
    {
        return false;
    }
    f.print(configToJson());
    f.close();
    return true;
}

void handleBackupSave()
{
    if (!requireAuth())
    {
        return;
    }
    if (!sdReady)
    {
        server.send(400, "application/json", "{\"error\":\"sin tarjeta SD\"}");
        return;
    }
    String path;
    if (saveConfigBackup(path))
    {
        logLine("Backup de config guardado: " + path);
        server.send(200, "application/json", "{\"ok\":true,\"file\":\"" + backupBasename(path) + "\"}");
    }
    else
    {
        server.send(500, "application/json", "{\"error\":\"no se pudo escribir\"}");
    }
}

void handleBackupList()
{
    SdGuard _sd; // serializa el acceso a SD contra la tarea logger (core0)
    String json = "[";
    if (sdReady && SD.exists(BACKUP_DIR))
    {
        File dir = SD.open(BACKUP_DIR);
        bool first = true;
        for (File e = dir.openNextFile(); e; e = dir.openNextFile())
        {
            if (!e.isDirectory())
            {
                if (!first)
                {
                    json += ",";
                }
                json += "{\"name\":\"" + jsonEscape(backupBasename(String(e.name()))) + "\",\"size\":" + String(e.size()) + "}";
                first = false;
            }
        }
        dir.close();
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleRestore()
{
    if (!requireAuth())
    {
        return;
    }
    if (!sdReady)
    {
        server.send(400, "application/json", "{\"error\":\"sin tarjeta SD\"}");
        return;
    }
    const String base = backupBasename(server.arg("file"));
    if (base.length() == 0)
    {
        server.send(400, "application/json", "{\"error\":\"falta el archivo\"}");
        return;
    }
    String json;
    {
        SdGuard _sd; // serializa la lectura de SD; el guard libera al salir del bloque (incluido el return)
        File f = SD.open(String(BACKUP_DIR) + "/" + base, FILE_READ);
        if (!f)
        {
            server.send(404, "application/json", "{\"error\":\"no existe\"}");
            return;
        }
        json = f.readString();
        f.close();
    }
    // Toma el mutex de config antes de mutar los String (la pollTask lo respeta -> race cerrado).
    xSemaphoreTake(configMutex, portMAX_DELAY);
    if (!applyConfigFromJson(json))
    {
        // JSON invalido: libera el mutex antes de salir.
        xSemaphoreGive(configMutex);
        server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
        return;
    }
    const bool okSave = saveConfig();
    xSemaphoreGive(configMutex);
    if (!okSave)
    {
        logAt(LVL_ERROR, "NVS: fallo al guardar la config restaurada; no se reinicia");
        server.send(500, "application/json", "{\"error\":\"no se pudo guardar en NVS\"}");
        return; // no reinicia: al arrancar loadConfig revertiria y se perderia el cambio en silencio
    }
    logLine("Config restaurada de " + base + "; reiniciando");
    server.send(200, "application/json", "{\"ok\":true}");
    webOtaRestartPending = true;
    webOtaRestartAtMs = millis() + 800;
}

// Lista los archivos de log de la SD, del mas reciente (actual) al mas antiguo (.5).
// Motivo del ultimo reinicio, en texto (para el log persistente en SD).
const char *resetReasonStr(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON: return "encendido";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "PANIC / crash";
    case ESP_RST_INT_WDT: return "watchdog de interrupcion";
    case ESP_RST_TASK_WDT: return "watchdog de tarea (loop colgado)";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (bajon de voltaje)";
    case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
    case ESP_RST_EXT: return "reset externo";
    default: return "desconocido";
    }
}

// Guarda a la SD el core dump del crash previo (si hay), con LIMITE de COREDUMP_KEEP archivos
// (slot rotativo cd-0..cd-(KEEP-1)), y borra el de flash para no re-guardarlo cada arranque.
void saveCoreDumpToSd()
{
    if (esp_core_dump_image_check() != ESP_OK)
    {
        return; // no hay volcado valido
    }
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0 || size > 0x10000)
    {
        esp_core_dump_image_erase();
        return;
    }
    logAt(LVL_ERROR, "Core dump del crash previo: " + String((uint32_t)size) + " bytes");
    bool saved = false;
    if (sdReady)
    {
        SdGuard _sd; // serializa el acceso a SD (aunque en boot aun no corre la tarea logger)
        if (!SD.exists(COREDUMP_DIR))
        {
            SD.mkdir(COREDUMP_DIR);
        }
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (p)
        {
            const String path = String(COREDUMP_DIR) + "/cd-" + String(bootCount % COREDUMP_KEEP) + ".elf";
            File f = SD.open(path, FILE_WRITE);
            if (f)
            {
                bool ok = true;
                size_t written = 0;
                uint8_t buf[512];
                for (size_t off = 0; off < size; off += sizeof(buf))
                {
                    const size_t chunk = (size - off < sizeof(buf)) ? (size - off) : sizeof(buf);
                    if (esp_partition_read(p, off, buf, chunk) != ESP_OK || f.write(buf, chunk) != chunk)
                    {
                        ok = false;
                        break;
                    }
                    written += chunk;
                }
                f.close();
                saved = ok && (written == size);
                logAt(saved ? LVL_WARN : LVL_ERROR,
                      saved ? "Core dump guardado en SD: " + path + " (decodifica con espcoredump.py)"
                            : "Fallo guardando el core dump en SD; se conserva en flash para el proximo arranque");
            }
        }
    }
    // Solo limpia la flash si el volcado quedo PERSISTIDO en SD. Si no hay SD o fallo la escritura,
    // se conserva en flash para reintentar en el proximo arranque y no perder el diagnostico.
    if (saved)
    {
        esp_core_dump_image_erase();
    }
}

void handleFilesList()
{
    SdGuard _sd; // serializa el acceso a SD contra la tarea logger (core0)
    String json = "[";
    bool first = true;
    if (sdReady)
    {
        static const char *const NAMES[] = {"/cyd-log.txt", "/cyd-log.1.txt", "/cyd-log.2.txt", "/cyd-log.3.txt", "/cyd-log.4.txt", "/cyd-log.5.txt"};
        for (uint8_t i = 0; i < 6; i++)
        {
            if (SD.exists(NAMES[i]))
            {
                File f = SD.open(NAMES[i], FILE_READ);
                if (f)
                {
                    if (!first)
                    {
                        json += ",";
                    }
                    json += "{\"name\":\"" + String(NAMES[i]).substring(1) + "\",\"size\":" + String(f.size()) + "}";
                    f.close();
                    first = false;
                }
            }
        }
        // Core dumps de crashes (para descargar y decodificar el backtrace).
        if (SD.exists(COREDUMP_DIR))
        {
            File dir = SD.open(COREDUMP_DIR);
            if (dir)
            {
                for (File e = dir.openNextFile(); e; e = dir.openNextFile())
                {
                    if (!e.isDirectory())
                    {
                        if (!first)
                        {
                            json += ",";
                        }
                        json += "{\"name\":\"" + jsonEscape(backupBasename(String(e.name()))) + "\",\"size\":" + String(e.size()) + ",\"dump\":true}";
                        first = false;
                    }
                }
                dir.close();
            }
        }
    }
    json += "]";
    server.send(200, "application/json", json);
}

// Descarga un archivo de la SD (attachment): logs cyd-log* (texto) o core dumps cd-* (binario).
void handleDownload()
{
    SdGuard _sd; // serializa el acceso a SD contra la tarea logger durante todo el streamFile
    if (!sdReady)
    {
        server.send(400, "text/plain", "sin tarjeta SD");
        return;
    }
    const String name = backupBasename(server.arg("f"));
    String path;
    const bool isDump = name.startsWith("cd-");
    if (name.startsWith("cyd-log"))
    {
        path = "/" + name;
    }
    else if (isDump)
    {
        path = String(COREDUMP_DIR) + "/" + name;
    }
    else
    {
        server.send(403, "text/plain", "no permitido");
        return;
    }
    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        server.send(404, "text/plain", "no existe");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, isDump ? "application/octet-stream" : "text/plain");
    f.close();
}

// Preview de un screensaver en la pantalla del CYD (sin guardar config), por unos segundos.
void handleSaverTest()
{
    if (!requireAuth())
    {
        return;
    }
    const uint8_t id = (uint8_t)server.arg("id").toInt();
    saverPreviewId = (id >= 1 && id <= 4) ? id : 1;
    saverPreviewUntilMs = millis() + 12000;
    server.send(200, "application/json", "{\"ok\":true}");
}

void handlePostConfig()
{
    if (!requireAuth())
    {
        return;
    }
    // Toma el mutex de config mientras se mutan los String: la pollTask (core0) lo respeta al
    // copiar serviceUrl/pollInterval, cerrando el race sin suspender la tarea (evita el use-
    // after-free del buffer del String durante la reasignacion cross-core).
    xSemaphoreTake(configMutex, portMAX_DELAY);
    if (server.hasArg("ssid"))
    {
        config.wifiSsid = server.arg("ssid");
    }
    if (server.hasArg("pass") && server.arg("pass").length() > 0)
    {
        config.wifiPassword = server.arg("pass");
    }
    if (server.hasArg("service"))
    {
        config.serviceUrl = server.arg("service");
    }
    if (server.hasArg("poll"))
    {
        config.pollIntervalMs = server.arg("poll").toInt();
    }
    if (server.hasArg("rot"))
    {
        config.rotation = server.arg("rot").toInt();
    }
    if (server.hasArg("bright"))
    {
        config.brightnessPct = server.arg("bright").toInt();
    }
    if (server.hasArg("dimb"))
    {
        config.dimBrightnessPct = server.arg("dimb").toInt();
    }
    if (server.hasArg("dima"))
    {
        config.dimAfterSec = server.arg("dima").toInt();
    }
    if (server.hasArg("inv"))
    {
        config.invertDisplay = server.arg("inv") == "1";
    }
    if (server.hasArg("aspd"))
    {
        config.animSpeed = server.arg("aspd").toInt();
    }
    if (server.hasArg("abx"))
    {
        config.animBoxX = server.arg("abx").toInt();
    }
    if (server.hasArg("wt"))
    {
        config.waitTitle = server.arg("wt");
    }
    if (server.hasArg("wm"))
    {
        config.waitMsg = server.arg("wm");
    }
    if (server.hasArg("wpass") && server.arg("wpass").length() > 0)
    {
        config.webPassword = server.arg("wpass");
    }
    if (server.hasArg("sav"))
    {
        config.screensaver = server.arg("sav").toInt();
    }
    if (server.hasArg("ccol"))
    {
        config.claudeColor = server.arg("ccol");
    }
    if (server.hasArg("xcol"))
    {
        config.codexColor = server.arg("xcol");
    }
    if (server.hasArg("tcol"))
    {
        config.textColor = server.arg("tcol");
    }
    if (server.hasArg("mcol"))
    {
        config.mutedColor = server.arg("mcol");
    }
    if (server.hasArg("twin"))
    {
        config.trendWindowMin = server.arg("twin").toInt();
    }
    if (server.hasArg("tmar"))
    {
        config.trendMarginPct = server.arg("tmar").toInt();
    }
    if (server.hasArg("asq"))
    {
        config.animSquarePx = server.arg("asq").toInt();
    }
    if (server.hasArg("bgc"))
    {
        config.bgColor = server.arg("bgc");
    }
    if (server.hasArg("dvc"))
    {
        config.dividerColor = server.arg("dvc");
    }
    if (server.hasArg("bbc"))
    {
        config.barBgColor = server.arg("bbc");
    }
    if (server.hasArg("gdc"))
    {
        config.goodColor = server.arg("gdc");
    }
    if (server.hasArg("wnc"))
    {
        config.warnColor = server.arg("wnc");
    }
    if (server.hasArg("bdc"))
    {
        config.badColor = server.arg("bdc");
    }
    if (server.hasArg("dsc"))
    {
        config.disabledColor = server.arg("dsc");
    }
    if (server.hasArg("alrt"))
    {
        config.alertThresholdPct = server.arg("alrt").toInt();
    }
    if (server.hasArg("clbl"))
    {
        config.claudeLabel = server.arg("clbl");
    }
    if (server.hasArg("xlbl"))
    {
        config.codexLabel = server.arg("xlbl");
    }
    if (server.hasArg("asty"))
    {
        config.animStyle = server.arg("asty").toInt();
    }
    if (server.hasArg("ahz"))
    {
        config.animHz = server.arg("ahz").toInt();
    }
    if (server.hasArg("ablk"))
    {
        config.alertBlinkMs = server.arg("ablk").toInt();
    }
    sanitizeConfig(); // acota los valores antes de persistir (evita brillo=0, animBoxX invalido, etc.)
    const bool okSave = saveConfig();
    xSemaphoreGive(configMutex);
    if (!okSave)
    {
        logAt(LVL_ERROR, "NVS: fallo al guardar la config; no se reinicia");
        server.send(500, "application/json", "{\"status\":\"error\",\"error\":\"no se pudo guardar en NVS\"}");
        return; // no reinicia: evitaria confirmar un guardado que en realidad no ocurrio
    }
    logLine("Config guardada por web; reiniciando");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    webOtaRestartPending = true;
    webOtaRestartAtMs = millis() + 800;
}

void handleUpdateUpload()
{
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START)
    {
        // Auth: sin credenciales validas no se inicia la escritura del firmware.
        if (config.webPassword.length() > 0 && !server.authenticate(WEB_USER, config.webPassword.c_str()))
        {
            webOtaError = true;
            webOtaStatus = "No autorizado.";
            return;
        }
        webOtaError = false;
        webOtaStatus = "Subiendo...";
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            webOtaError = true;
            webOtaStatus = String("Error OTA: ") + Update.errorString();
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        // La subida completa se procesa dentro de UNA sola vuelta de server.handleClient(), sin
        // volver al loop: hay que alimentar el watchdog aqui o un firmware grande (>15s por WiFi
        // lento) dispararia el WDT y reiniciaria a mitad del flasheo.
        esp_task_wdt_reset();
        if (!webOtaError && Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        {
            webOtaError = true;
            webOtaStatus = String("Error OTA: ") + Update.errorString();
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (!webOtaError && Update.end(true))
        {
            webOtaStatus = "Firmware actualizado. Reiniciando...";
            logLine("OTA web OK; reiniciando");
            webOtaRestartPending = true;
            webOtaRestartAtMs = millis() + 1500;
        }
        else
        {
            webOtaStatus = String("Error OTA: ") + Update.errorString();
            webOtaError = true;
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        // El cliente corto la subida: abortar el Update para no dejar el singleton
        // "corriendo" y bloquear futuras OTA hasta reiniciar.
        Update.abort();
        webOtaError = true;
        webOtaStatus = "Subida cancelada.";
    }
}

void handleUpdateDone()
{
    if (!requireAuth())
    {
        return;
    }
    server.send(200, "text/plain", webOtaError ? "ERROR" : "OK");
}

// Se declara arriba (getActiveWifiSsidSafe) para el status JSON.
String getActiveWifiSsidSafe()
{
    if (emergencyWifiActive)
    {
        return String(EMERGENCY_AP_SSID);
    }
    return WiFi.SSID();
}

void setupWebServer()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/logs", HTTP_GET, handleGetLogs);
    server.on("/api/backups", HTTP_GET, handleBackupList);
    server.on("/api/backup", HTTP_POST, handleBackupSave);
    server.on("/api/restore", HTTP_POST, handleRestore);
    server.on("/api/files", HTTP_GET, handleFilesList);
    server.on("/api/download", HTTP_GET, handleDownload);
    server.on("/api/saver", HTTP_POST, handleSaverTest);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.begin();
}

void setupOta()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0)
    {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }
    // ArduinoOTA.handle() bloquea el loop toda la transferencia sin volver a alimentar el WDT.
    // Alimentarlo en cada progreso (y al arrancar) evita que un OTA >15s dispare el reinicio.
    ArduinoOTA.onStart([]()
                       { esp_task_wdt_reset(); });
    ArduinoOTA.onProgress([](unsigned int, unsigned int)
                          { esp_task_wdt_reset(); });
    ArduinoOTA.begin();
}

// ---------------------------------------------------------------------------
// setup / loop.
// ---------------------------------------------------------------------------

void setupPalette()
{
    // Paleta completa desde la config (todos configurables desde la web).
    pal.bg = hexTo565(config.bgColor);
    pal.panelDivider = hexTo565(config.dividerColor);
    pal.textPrimary = hexTo565(config.textColor);
    pal.textMuted = hexTo565(config.mutedColor);
    pal.barBg = hexTo565(config.barBgColor);
    pal.good = hexTo565(config.goodColor);
    pal.warn = hexTo565(config.warnColor);
    pal.bad = hexTo565(config.badColor);
    pal.muted = hexTo565(config.disabledColor);
    pal.claude = hexTo565(config.claudeColor);
    pal.codex = hexTo565(config.codexColor);
    pal.ok = tft.color565(90, 160, 235);   // info (uso interno menor)
    pal.tick = tft.color565(220, 224, 230); // marca (uso interno menor)
}

// Registra el superavit actual de cada bloque en su historico y calcula la direccion de la
// tendencia (contra el superavit de hace config.trendWindowMin minutos, con margen muerto).
// Solo se llama con datos frescos (desde consumeIncomingState) y estando online.
void recordTrends()
{
    if (!monitorState.online)
    {
        return;
    }
    const uint32_t now = millis();
    const uint32_t windowMs = (uint32_t)config.trendWindowMin * 60000UL;
    // La flecha reacciona al uso RECIENTE: compara el margen actual contra el de hace ~reactMs
    // (max RECENT_TREND_MS, o la ventana configurada si es menor), no contra 60 min atras. Asi, al
    // acelerar el consumo, el margen vs el ritmo baja pronto -> flecha abajo, en vez de que el
    // colchon acumulado la deje siempre arriba.
    const uint32_t reactMs = windowMs < RECENT_TREND_MS ? windowMs : RECENT_TREND_MS;
    uint32_t sampleEvery = reactMs / (TREND_SAMPLES - 1);
    if (sampleEvery < 15000UL)
    {
        sampleEvery = 15000UL; // no muestrear mas rapido que cada 15s
    }
    const WindowData *wins[TREND_BLOCKS] = {
        &monitorState.claude.current, &monitorState.claude.weekly,
        &monitorState.codex.current, &monitorState.codex.weekly};
    const bool enabled[TREND_BLOCKS] = {
        monitorState.claude.enabled, monitorState.claude.enabled,
        monitorState.codex.enabled, monitorState.codex.enabled};

    for (uint8_t b = 0; b < TREND_BLOCKS; b++)
    {
        if (!enabled[b])
        {
            trendDir[b] = 0;
            continue;
        }
        const float surplus = wins[b]->remaining - wins[b]->expected;
        TrendHist &h = trendHist[b];
        // Registra una muestra si paso el intervalo (o es la primera).
        const uint32_t lastMs = h.count ? h.s[(h.head + TREND_SAMPLES - 1) % TREND_SAMPLES].ms : 0;
        if (h.count == 0 || now - lastMs >= sampleEvery)
        {
            h.s[h.head] = {now, surplus};
            h.head = (h.head + 1) % TREND_SAMPLES;
            if (h.count < TREND_SAMPLES)
            {
                h.count++;
            }
        }
        // Direccion: superavit actual vs la muestra mas cercana a (now - reactMs), exigiendo
        // que tenga al menos media ventana reactiva de antiguedad (si no, aun no hay historia).
        float past = 0.0f;
        bool found = false;
        uint32_t bestDiff = UINT32_MAX;
        for (uint8_t i = 0; i < h.count; i++)
        {
            const uint32_t age = now - h.s[i].ms;
            if (age < reactMs / 2)
            {
                continue;
            }
            const uint32_t diff = age > reactMs ? age - reactMs : reactMs - age;
            if (diff < bestDiff)
            {
                bestDiff = diff;
                past = h.s[i].surplus;
                found = true;
            }
        }
        if (!found)
        {
            trendDir[b] = 0;
            continue;
        }
        const float d = surplus - past;
        const float margin = (float)config.trendMarginPct;
        trendDir[b] = d > margin ? 1 : (d < -margin ? -1 : 0);
    }
}

// Consume el estado publicado por la tarea de red y dibuja (solo aqui se toca la TFT).
void consumeIncomingState()
{
    if (!incomingReady)
    {
        return;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool wasOnline = monitorState.online;
    const uint32_t prevActivityChange = monitorState.lastActivityChangeMs;
    const String prevClaudeAct = monitorState.claude.activity;
    const String prevCodexAct = monitorState.codex.activity;
    monitorState = incomingState;
    incomingReady = false;
    xSemaphoreGive(stateMutex);

    // Conserva la marca de cambio de actividad (para el atenuado anti burn-in).
    if (monitorState.claude.activity != prevClaudeAct || monitorState.codex.activity != prevCodexAct)
    {
        monitorState.lastActivityChangeMs = millis();
    }
    else
    {
        monitorState.lastActivityChangeMs = prevActivityChange;
    }

    // Actualiza el historico/tendencia del superavit con los datos frescos.
    recordTrends();

    // Log de transiciones de servicio (para verlas en tiempo real desde la web).
    if (monitorState.online && !wasOnline)
    {
        logLine("Servicio en linea");
    }
    else if (!monitorState.online && wasOnline)
    {
        logAt(LVL_WARN, "Servicio caido: " + (monitorState.lastError.length() > 0 ? monitorState.lastError : String("sin conexion")));
    }
    // DEBUG: transiciones de actividad (filtrado por defecto en la web).
    if (monitorState.claude.activity != prevClaudeAct || monitorState.codex.activity != prevCodexAct)
    {
        logAt(LVL_DEBUG, "Actividad claude=" + monitorState.claude.activity + " codex=" + monitorState.codex.activity);
    }

    if (infoOverlayActive || saverActive)
    {
        return; // Overlay o screensaver arriba: no se dibuja el monitor (evita superposicion).
    }

    if (monitorState.online)
    {
        if (!wasOnline)
        {
            needsFullRedraw = true;
        }
        renderMonitor();
    }
    else if (wasOnline || needsFullRedraw)
    {
        // Con screensaver activo, el saver toma la pantalla (no el aviso estatico).
        if (config.screensaver == 0)
        {
            drawOfflineScreen(monitorState.lastError.length() > 0 ? monitorState.lastError : "Sin conexion");
        }
        else
        {
            needsFullRedraw = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Overlay de diagnostico al tocar la pantalla.
// ---------------------------------------------------------------------------

String sinceStr(uint32_t ts)
{
    if (ts == 0)
    {
        return "-";
    }
    return String((millis() - ts) / 1000) + "S";
}

void infoLine(int16_t &y, const String &label, const String &value, uint16_t valColor)
{
    // Dibuja en el sprite; y es absoluto en la pantalla (el offset de banda ya se resto).
    drawPixelText(panelSprite, 8, y, label, 2, pal.textMuted);
    drawPixelText(panelSprite, 8 + pixelTextWidth(label, 2) + 8, y, value, 2, valColor);
    y += 16;
}

// Renderiza el overlay en una banda de PANEL_H px (via el sprite) y la vuelca. El
// contenido se dibuja con las coordenadas absolutas menos bandTop; el sprite recorta.
void drawInfoOverlayBand(int16_t bandTop)
{
    panelSprite.fillSprite(pal.bg);
    drawPixelText(panelSprite, 8, 6 - bandTop, "INFO / DEBUG", 3, pal.claude);
    int16_t y = 34 - bandTop;
    infoLine(y, "IP", WiFi.localIP().toString(), pal.textPrimary);
    infoLine(y, "WIFI", WiFi.SSID() + " " + String(WiFi.RSSI()) + "DBM", pal.textPrimary);
    infoLine(y, "SVC", config.serviceUrl, pal.textPrimary);
    infoLine(y, "ONLINE", monitorState.online ? "SI" : "NO", monitorState.online ? pal.good : pal.bad);
    infoLine(y, "ULT HTTP", sinceStr(statLastPollMs), pal.textPrimary);
    infoLine(y, "ULT OK", sinceStr(statLastSuccessMs), pal.textPrimary);
    infoLine(y, "POLLS", String(statPollCount) + " / FALLOS " + String(statFailCount), pal.textPrimary);
    infoLine(y, "SD", sdReady ? String((uint32_t)(SD.cardSize() / (1024 * 1024))) + "MB" : "NO", sdReady ? pal.good : pal.warn);
    infoLine(y, "SESIONES", String("C") + String(claudeSessions) + " X" + String(codexSessions), pal.textPrimary);
    infoLine(y, "ACT", monitorState.claude.activity + " / " + monitorState.codex.activity, pal.textPrimary);
    infoLine(y, "ERROR", monitorState.lastError.length() > 0 ? monitorState.lastError : "-", pal.warn);
    infoLine(y, "HEAP", String(ESP.getFreeHeap()) + "B", pal.textPrimary);
    infoLine(y, "UPTIME", String(millis() / 1000) + "S", pal.textPrimary);
    panelSprite.pushSprite(0, bandTop);
}

void drawInfoOverlay()
{
    // Dos bandas (arriba/abajo) volcadas desde el sprite: sin flicker y con datos vivos.
    drawInfoOverlayBand(0);
    drawInfoOverlayBand(PANEL_H);
}

// Gestiona el overlay: cada toque (flanco) lo ALTERNA on/off. Mientras esta activo se
// refresca 1 vez por segundo (sin flicker) para actualizar los contadores.
void handleTouch()
{
    static bool prevTouched = false;
    static uint32_t lastToggleMs = 0;
    const bool touched = (digitalRead(TOUCH_IRQ_PIN) == LOW);
    const uint32_t now = millis();

    if (touched && !prevTouched && (now - lastToggleMs) > 300)
    {
        lastToggleMs = now;
        infoOverlayActive = !infoOverlayActive;
        if (infoOverlayActive)
        {
            drawInfoOverlay();
            lastOverlayDrawMs = now;
        }
        else
        {
            needsFullRedraw = true;
            if (monitorState.online)
            {
                renderMonitor();
            }
            else if (config.screensaver != 0)
            {
                saverActive = false; // el screensaver se reinicia (limpia) en la proxima vuelta
            }
            else
            {
                drawOfflineScreen(monitorState.lastError.length() > 0 ? monitorState.lastError : "Sin conexion");
            }
        }
    }
    prevTouched = touched;

    if (infoOverlayActive && (now - lastOverlayDrawMs) >= 1000)
    {
        drawInfoOverlay();
        lastOverlayDrawMs = now;
    }
}

void setup()
{
    Serial.begin(115200);

    // Cola de logs + mutex de SD ANTES de cualquier log: asi las lineas de arranque tambien se
    // encolan y persisten a SD (las drena la tarea logger, creada junto a pollTask mas abajo).
    logQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogEntry));
    sdMutex = xSemaphoreCreateMutex();

    // Apaga el LED RGB del CYD (activo en bajo).
    for (uint8_t i = 0; i < sizeof(RGB_LED_PINS); i++)
    {
        pinMode(RGB_LED_PINS[i], OUTPUT);
        digitalWrite(RGB_LED_PINS[i], HIGH);
    }

    // PENIRQ del tactil como entrada (deteccion de toque en cualquier lado).
    pinMode(TOUCH_IRQ_PIN, INPUT);

    loadConfig();

    tft.init();
    tft.setRotation(config.rotation);
    // El panel ILI9341 del CYD muestra los colores invertidos con la config estandar;
    // se corrige con la inversion de hardware (configurable desde la web por si acaso).
    tft.invertDisplay(config.invertDisplay);
    setupPalette();

    // Retroiluminacion por PWM. Se ATA en 0 (apagado), se pinta el fondo y RECIEN se sube el
    // brillo: asi no se ve el flash blanco/basura de la VRAM sin inicializar del ILI9341.
    ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ, BL_LEDC_BITS);
    ledcAttachPin(TFT_BL, BL_LEDC_CHANNEL);
    applyBrightness(0);
    tft.fillScreen(pal.bg);
    applyBrightness(config.brightnessPct);

    // ID unico + contador consecutivo de arranque (persistido en NVS).
    bootId = makeBootId();
    preferences.begin("cydmon", false);
    bootCount = preferences.getUInt("bootn", 0) + 1;
    preferences.putUInt("bootn", bootCount);
    preferences.end();

    monitorState.online = false;
    monitorState.lastActivityChangeMs = millis();
    monitorState.claude.activity = "idle";
    monitorState.codex.activity = "idle";
    offlineSinceMs = millis(); // arranca el reloj de gracia (evita matrix inmediato al bootear)

    // Conecta WiFi y sincroniza NTP ANTES de loguear, para que los timestamps sean la fecha
    // real desde la primera linea (no el epoch). Muestra pantallas de progreso.
    const bool wifiOk = connectWifi();
    if (wifiOk)
    {
        configTime(-5 * 3600, 0, "pool.ntp.org", "time.google.com"); // America/Bogota GMT-5
        waitForNtp(6000);
    }

    // Crea la tarea de red (core 0) ANTES de reservar los sprites: asi su stack (16KB) obtiene un
    // bloque contiguo con el heap aun fresco. Si se crea despues de los ~112KB de sprites, no hay
    // 16KB seguidos libres y xTaskCreate falla en silencio -> nunca sondea (hb/polls=0).
    stateMutex = xSemaphoreCreateMutex();
    configMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(pollTask, "pollTask", 16384, nullptr, 1, &pollTaskHandle, 0);
    // Tarea logger (core0): drena la cola de logs y los vuelca a la SD, fuera del path de render
    // (antes cada transicion/actividad bloqueaba el core1 ~10-50ms en SPI de la SD). Se crea aqui,
    // con el heap aun fresco, para que su stack quede contiguo (igual que pollTask).
    xTaskCreatePinnedToCore(loggerTask, "loggerTask", 4096, nullptr, 1, nullptr, 0);

    // Solo el sprite del panel es permanente (76KB). Los sprites de la alerta, la franja busy y
    // el DVD se reservan BAJO DEMANDA y se liberan al salir del estado: asi en operacion normal
    // (idle online) queda el maximo de heap libre para el parseo del snapshot (evita JSON:NoMemory).
    panelSprite.setColorDepth(16);
    spriteReady = (panelSprite.createSprite(SCREEN_W, PANEL_H) != nullptr);
    littleFsReady = LittleFS.begin(true); // formatea si hace falta
    sdSpi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdReady = SD.begin(SD_CS_PIN, sdSpi);

    // --- Consola de arranque (ya con hora real) ---
    bootLog("BOOT #" + String(bootCount) + " id=" + bootId, pal.textPrimary);
    bootLog("POST pantalla ILI9341: OK", pal.good);
    bootLog(spriteReady ? "POST sprite panel 320x120: OK" : "POST sprite: FALLO (sin memoria)", spriteReady ? pal.good : pal.bad);
    bootLog("Heap libre: " + String(ESP.getFreeHeap()) + "B", pal.textMuted);
    bootLog(littleFsReady ? "LittleFS: montado (" + String((uint32_t)(LittleFS.totalBytes() / 1024)) + "KB)" : "LittleFS: fallo", littleFsReady ? pal.good : pal.warn);
    bootLog(sdReady ? "SD: " + String((uint32_t)(SD.cardSize() / (1024 * 1024))) + "MB -> logs a SD" : "SD: no detectada -> logs en RAM", sdReady ? pal.good : pal.warn);
    // Motivo del ultimo reinicio (persistido en el log de SD) y volcado del crash previo si hubo.
    const esp_reset_reason_t rr = esp_reset_reason();
    const bool crash = (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_BROWNOUT);
    bootLog("Reinicio: " + String(resetReasonStr(rr)), crash ? pal.bad : pal.textMuted);
    saveCoreDumpToSd();
    bootLog("Config NVS: cargada", pal.good);
    bootLog("Servicio: " + config.serviceUrl, pal.textMuted);

    if (wifiOk)
    {
        bootLog("WiFi: " + config.wifiSsid + " OK", pal.good);
        bootLog("IP " + WiFi.localIP().toString() + "  RSSI " + String(WiFi.RSSI()) + "DBM", pal.good);
        if (MDNS.begin(ESP_HOSTNAME))
        {
            MDNS.addService("http", "tcp", HTTP_PORT);
            bootLog("mDNS: " + String(LOCAL_DOMAIN), pal.good);
        }
        setupOta();
        bootLog("OTA: activo", pal.good);
        setupWebServer();
        bootLog("Web: http://" + WiFi.localIP().toString(), pal.good);
        bootLog("Tarea de red: core 0", pal.good); // (ya creada antes de los sprites)
        bootLog("Listo, esperando datos...", pal.claude);
    }
    else
    {
        bootLog(config.wifiSsid.length() == 0 ? "WiFi: sin SSID configurado" : "WiFi: FALLO al conectar", pal.bad);
        setupWebServer();
        startEmergencyAp();
    }

    // Watchdog del loop (core1): si una vuelta tarda >15s (bloqueo en mutex, fault, corrupcion),
    // reinicia solo para recuperarse en vez de quedarse congelado en el screensaver.
    esp_task_wdt_init(15, true);
    esp_task_wdt_add(NULL);
}

void loop()
{
    esp_task_wdt_reset(); // alimenta el watchdog: si el loop se cuelga, reinicia y se recupera solo
    // Ritmo del loop (proxy de CPU): vueltas por segundo, calculado cada 1s (costo nulo).
    loopCount++;
    if (millis() - lastHzMs >= 1000)
    {
        loopHz = loopCount - lastHzCount;
        lastHzCount = loopCount;
        lastHzMs = millis();
    }

    server.handleClient();
    if (!emergencyWifiActive)
    {
        ArduinoOTA.handle();
    }

    if (webOtaRestartPending && millis() >= webOtaRestartAtMs)
    {
        ESP.restart();
    }

    if (emergencyWifiActive)
    {
        return;
    }

    // Watchdog de la tarea de red (core0): el WDT de tarea solo vigila este loop (core1). Si
    // pollTask se cuelga (mutex huerfano, wedge de socket/lwip pese a los timeouts), el loop
    // seguiria vivo y el CYD quedaria congelado con datos viejos SIN reiniciar. Cierra el lazo del
    // latido: si con WiFi conectado pollHeartbeat no avanza en POLL_STALL_MS (mayor que el peor
    // long-poll ~60s, para no dar falsos positivos), la tarea esta atascada -> reinicia y recupera.
    static uint32_t lastHb = 0;
    static uint32_t lastHbMs = 0;
    const uint32_t hbNow = millis();
    if (pollHeartbeat != lastHb || lastHbMs == 0)
    {
        lastHb = pollHeartbeat;
        lastHbMs = hbNow;
    }
    else if (WiFi.status() == WL_CONNECTED && hbNow - lastHbMs > POLL_STALL_MS)
    {
        logAt(LVL_ERROR, "pollTask sin latido " + String((hbNow - lastHbMs) / 1000) + "s -> reinicio de recuperacion");
        delay(50); // deja salir el log a SD antes de reiniciar
        ESP.restart();
    }

    // Reconexion WiFi simple (la tarea de red espera mientras tanto).
    static uint32_t lastWifiReconnectMs = 0;
    if (WiFi.status() != WL_CONNECTED)
    {
        // Avisa una sola vez que se perdio el WiFi (no deja datos viejos como vigentes).
        if (!wifiDownDrawn)
        {
            monitorState.online = false;
            if (config.screensaver == 0)
            {
                drawOfflineScreen("Sin WiFi");
            }
            logAt(LVL_WARN, "WiFi perdido; reconectando");
            wifiDownDrawn = true;
            WiFi.reconnect();
            lastWifiReconnectMs = millis();
        }
        else if (millis() - lastWifiReconnectMs > 8000)
        {
            // Reintenta con backoff (8s), NO cada vuelta: llamar reconnect() cada 200ms reinicia
            // la asociacion/DHCP en curso y alarga la recuperacion.
            WiFi.reconnect();
            lastWifiReconnectMs = millis();
        }
        // Permite gestionar el toque (abrir/cerrar el overlay) tambien sin WiFi: si no, un overlay
        // abierto quedaria congelado y sin poder cerrarse durante toda la caida, y bloqueando el
        // screensaver. handleTouch tambien lo refresca (mostrando que esta offline).
        handleTouch();
        // Libera los sprites de estados online (busy/waiting) que no aplican offline: el bloque
        // freeSprite del final del loop no se alcanza por el return de abajo, asi que sin esto se
        // retendrian ~33KB de heap durante toda la caida.
        freeSprite(stripSprite, stripReady);
        freeSprite(ovlSprite, ovlReady);
        // Sin WiFi tambien corre el screensaver (anti burn-in) en vez de quedar estatico.
        if (!infoOverlayActive)
        {
            runScreensaver();
        }
        delay(200);
        return;
    }
    if (wifiDownDrawn)
    {
        // Se recupero el enlace: fuerza un redibujado completo en la proxima publicacion.
        wifiDownDrawn = false;
        needsFullRedraw = true;
        saverActive = false;
        logLine("WiFi recuperado: " + WiFi.localIP().toString());
    }

    // Toque de pantalla: muestra/oculta el overlay de diagnostico.
    handleTouch();

    // Aplica el estado sondeado por la tarea y anima; el render nunca se bloquea por la red.
    // (consumeIncomingState y animateActivity no dibujan mientras el overlay esta arriba).
    consumeIncomingState();

    // Mientras hay servicio, resetea el reloj de gracia offline.
    if (monitorState.online)
    {
        offlineSinceMs = millis();
    }

    // Preview forzado desde la web (prioritario), o screensaver offline tras la gracia
    // (para no lanzarlo al arrancar mientras llega el primer dato).
    const bool previewSaver = (saverPreviewId != 0 && millis() < saverPreviewUntilMs);
    const bool offlineSaver = (!monitorState.online && config.screensaver != 0 && millis() - offlineSinceMs >= SAVER_GRACE_MS);
    if (!infoOverlayActive && (previewSaver || offlineSaver))
    {
        runScreensaver();
    }
    else
    {
        if (saverActive)
        {
            // Se salio del screensaver: redibuja el monitor/offline.
            saverActive = false;
            needsFullRedraw = true;
            if (monitorState.online)
            {
                renderMonitor();
            }
        }
        animateActivity();
    }
    updateBrightness();
    rampBrightness(); // converge suave hacia el objetivo (rampa anti-escalon)

    // Libera los sprites que no se usan ahora (deja el maximo heap libre para el snapshot JSON).
    const bool busyNow = monitorState.online && (monitorState.claude.activity == "busy" || monitorState.codex.activity == "busy");
    const bool waitNow = monitorState.online && (monitorState.claude.activity == "waiting" || monitorState.codex.activity == "waiting");
    if (!busyNow)
    {
        freeSprite(stripSprite, stripReady);
    }
    if (!waitNow)
    {
        freeSprite(ovlSprite, ovlReady);
    }
    if (!saverActive)
    {
        freeSprite(dvdSprite, dvdSpriteReady);
    }
}
