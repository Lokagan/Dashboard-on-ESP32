// ============================================================
// OTA_MANAGER.CPP — ArduinoOTA sur tâche FreeRTOS dédiée
// ArduinoOTA.handle() tourne sur le cœur 0 en parallèle de
// loop(), ce qui évite les timeouts causés par LVGL.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <ArduinoOTA.h>
#include <WiFi.h>

// ---- RESSOURCES LOCALES ----
#include "ota_manager.h"
#include "config.h"
#include "display_manager.h"
#include "audio_manager.h"
#include "led_manager.h"
#include "log_manager.h"

// ---- API LOCALES ----

static void _ota_task(void* pvParameters) {
    for (;;) {
        ArduinoOTA.handle();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- API PUBLIQUES ----

static int _ota_last_pct = 0;

void ota_init() {
    ArduinoOTA.setHostname(WIFI_HOSTNAME);

    #ifdef OTA_PASSWORD
        ArduinoOTA.setPassword(OTA_PASSWORD);
    #endif

    ArduinoOTA.onStart([]() {
        String type = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        log_line("[OTA] Debut mise a jour : %s", type.c_str());
        led_pause();
        display_pause();
        audio_click();
        display_show_ota_progress(0);
        _ota_last_pct = 0;
    });

    ArduinoOTA.onEnd([]() {
        log_line("[OTA] Termine — redemarrage...");
        display_show_ota_progress(100);
        delay(2000);
        ESP.restart();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int pct = (progress * 100) / total;
        if (pct == _ota_last_pct) return;   // ne rien faire tant que le % n'a pas bougé
        _ota_last_pct = pct;
        log_line("[OTA] Progression : %d%%", pct);
        display_show_ota_progress(pct);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        const char* desc = "Inconnue";
        switch (error) {
            case OTA_AUTH_ERROR:    desc = "Authentification"; break;
            case OTA_BEGIN_ERROR:   desc = "Begin";            break;
            case OTA_CONNECT_ERROR: desc = "Connexion";        break;
            case OTA_RECEIVE_ERROR: desc = "Reception";        break;
            case OTA_END_ERROR:     desc = "Fin";              break;
        }
        log_line("[OTA] Erreur [%u] : %s", error, desc);
        log_line("[OTA] Update.errorString() : %s", Update.errorString());
        Update.abort();   // évite qu'un begin() partiel ne pollue la tentative suivante
        display_show_ota_error(desc);
        audio_click();
        delay(5000);
        led_resume();
        display_resume();
    });

    ArduinoOTA.begin();

    // Tâche dédiée sur le cœur 0, priorité 2 (au-dessus de loop).
    xTaskCreatePinnedToCore(_ota_task, "ota_task", 4096, nullptr, 2, nullptr, 0);

    log_line("[OTA] Pret — hostname : %s.local", WIFI_HOSTNAME);
}

void ota_loop() {
    // Vide — le handle tourne sur la tâche dédiée.
    // Conservé pour ne pas casser l'API publique.
}
