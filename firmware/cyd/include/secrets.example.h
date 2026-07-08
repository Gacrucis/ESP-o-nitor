#pragma once

// Plantilla de credenciales locales del firmware.
//
// Copiar este archivo a secrets.h (misma carpeta) y poner los valores reales.
// secrets.h esta ignorado por git y nunca se versiona.
//
// Si no se crea secrets.h, el firmware compila con valores vacios: el CYD arranca
// en el AP de emergencia y se configura WiFi/servicio desde su pagina web.

#define SECRET_WIFI_SSID "YOUR_WIFI"
#define SECRET_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define SECRET_OTA_PASSWORD "YOUR_OTA_PASSWORD"

// URL por defecto del monitor-service (la IP de tu PC). Dejar vacio para configurarla
// despues desde la web del CYD. Ejemplo: "http://192.168.1.100:8765".
#define SECRET_SERVICE_URL ""

// Clave por defecto de la web de configuracion (usuario admin). Vacia = web abierta
// (uso en LAN de confianza). Se puede cambiar despues desde la propia web.
#define SECRET_WEB_PASSWORD ""
