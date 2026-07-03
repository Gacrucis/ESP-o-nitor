#pragma once

// Plantilla de credenciales locales del firmware.
//
// Copia este archivo a secrets.h (misma carpeta) y coloca tus valores reales.
// secrets.h esta ignorado por git y nunca se versiona.
//
// Si no creas secrets.h, el firmware compila con defaults vacios: el ESP arranca en el
// AP de emergencia y se configura WiFi/OTA desde su pagina web.

#define SECRET_WIFI_SSID "TU_WIFI"
#define SECRET_WIFI_PASSWORD "TU_CLAVE_WIFI"
#define SECRET_OTA_PASSWORD "TU_CLAVE_OTA"
