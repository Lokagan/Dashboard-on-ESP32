#pragma once

// ============================================================
// OTA_MANAGER.H — ArduinoOTA via WiFi
// ============================================================

// ---- API PUBLIQUES ----
void ota_init();   // À appeler dans setup() après wifi_connect()
void ota_loop();   // À appeler dans loop()
