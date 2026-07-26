// ============================================================
// WIFI_MANAGER.CPP — connexion WiFi et surveillance périodique de la
// liaison, avec reconnexion automatique.
//
// wifi_connect() est NON bloquant : la connexion se termine en
// arrière-plan, suivie par wifi_loop(). Le reste de setup() (OTA, MQTT,
// HTTP, LEDs, audio, IA) ne dépend donc pas du WiFi pour démarrer.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <WiFi.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "wifi_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
static unsigned long _last_check    = 0;
static bool          _was_connected = false;

// ---- API LOCALES ----

// Détecte connexion/déconnexion et relance si nécessaire.
static void _wifi_check() {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !_was_connected) {
        log_line("[WiFi] Connecté ! IP : %s", WiFi.localIP().toString().c_str());
    } else if (!connected) {
        if (_was_connected) log_line("[WiFi] Connexion perdue, reconnexion...");
        WiFi.reconnect();   // identifiants déjà configurés
    }
    _was_connected = connected;
}

// ---- API PUBLIQUES ----

void wifi_connect() {
    // Modem sleep désactivé : évite le retard sur les premiers paquets
    // OTA/MQTT après une période d'inactivité réseau.
    WiFi.setSleep(false);
    WiFi.setHostname(WIFI_HOSTNAME);
    log_line("[WiFi] Connexion à %s (en arrière-plan)...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void wifi_loop() {
    unsigned long now = millis();
    if (now - _last_check < WIFI_CHECK_INTERVAL_MS) return;
    _last_check = now;
    _wifi_check();
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}
