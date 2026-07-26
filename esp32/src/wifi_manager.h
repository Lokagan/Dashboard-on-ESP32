#pragma once

// ============================================================
// WIFI_MANAGER.H — connexion WiFi (station) + reconnexion auto.
// ============================================================

// ---- API PUBLIQUES ----
void wifi_connect();        // connexion initiale — NON bloquant, cf. .cpp
void wifi_loop();           // à appeler dans loop() — vérifie périodiquement
bool wifi_is_connected();
