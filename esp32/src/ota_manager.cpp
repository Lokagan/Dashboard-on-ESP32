// ============================================================
// OTA_MANAGER.CPP — ArduinoOTA sur tâche FreeRTOS dédiée, et ses écrans.
// ArduinoOTA.handle() tourne sur le cœur 0, ce qui évite les timeouts LVGL.
//
// Les écrans de progression et d'échec sont ICI, en prise directe sur la dalle
// — hors LVGL, en pause pendant le transfert. Même construction que
// sysinfo_manager : buffer PLEIN ÉCRAN hors-écran, coordonnées ABSOLUES,
// séparation FIXE / BLITTÉ.
//
// ORGANISATION DU FICHIER
//   1. SURFACE  — buffer hors-écran + poussée vers la dalle
//   2. DRAW     — primitives de dessin, famille unique
//   3. ÉCRANS   — progression et échec
//   4. OTA      — tâche, callbacks, API publiques
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

// ---- RESSOURCES LOCALES ----
#include "ota_manager.h"
#include "config.h"
#include "display_manager.h"
#include "display_driver.h"
#include "display_gfx.h"
#include "audio_manager.h"
#include "led_manager.h"
#include "wakeword_manager.h"
#include "mqtt_manager.h"
#include "log_manager.h"

// ════════════════════════════════════════════════════════════
// PALETTE
// ════════════════════════════════════════════════════════════

#define C_BG      0x0000
#define C_WHITE   0xFFFF
#define C_CYAN    0x07FF
#define C_DKCYAN  0x0410
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_DIM     0x4208


// ════════════════════════════════════════════════════════════
// GÉOMÉTRIE
// ════════════════════════════════════════════════════════════

#define OS_W        SCREEN_WIDTH
#define OS_H        SCREEN_HEIGHT
#define OS_MARGIN   24

#define RING_CX     (OS_W / 2)
#define RING_CY     124
#define RING_RO     60
#define RING_RI     40
#define RING_TOP    (RING_CY - RING_RO)          // zone BLITTÉE de la
#define RING_H      (RING_RO * 2 + 1)            // progression

#define TITLE_Y     40
#define BADGE_Y     190
#define RULE_TOP_Y  20
#define RULE_BOT_Y  220


// ════════════════════════════════════════════════════════════
// SURFACE — buffer hors-écran + poussée vers la dalle
// ════════════════════════════════════════════════════════════
// ⚠️ Alloué au PREMIER appel, rendu à la fin de l'OTA — jamais au boot : le
// plancher d'interne se creuse à l'init d'ESP_SR, une allocation postérieure
// ne l'abaisse pas.
//
// ⚠️ Buffer de dessin en PSRAM, mais la dalle est nourrie depuis un tampon de
// rebond en RAM INTERNE — seul chemin DMA-capable sur ce montage. La PSRAM
// n'est JAMAIS donnée directement à panel_flush().

namespace surface {

constexpr int BOUNCE_LINES = 8;

static uint16_t*   buf    = nullptr;   // 320x240, PSRAM
static uint16_t*   bounce = nullptr;   // 320x8,  interne
static gfx::Canvas cv     = { nullptr, OS_W, OS_H };

static bool ensure() {
    if (buf) return true;

    buf    = (uint16_t*)heap_caps_malloc((size_t)OS_W * OS_H * 2, MALLOC_CAP_SPIRAM);
    bounce = (uint16_t*)heap_caps_malloc((size_t)OS_W * BOUNCE_LINES * 2,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf || !bounce) {
        // Pas d'écran plutôt qu'un OTA qui échoue.
        log_line("[OTA] Surface indisponible — mise a jour sans ecran");
        free(buf); free(bounce);
        buf = bounce = nullptr;
        return false;
    }
    cv.px = buf;
    return true;
}

static void release() {
    free(buf); free(bounce);
    buf = bounce = nullptr;
    cv.px = nullptr;
}

// Rangées pleine largeur, par tranches de BOUNCE_LINES.
static void blit_rows(int y, int h) {
    if (!buf) return;
    for (int row = 0; row < h; row += BOUNCE_LINES) {
        int n = h - row;
        if (n > BOUNCE_LINES) n = BOUNCE_LINES;
        memcpy(bounce, buf + (size_t)(y + row) * OS_W, (size_t)n * OS_W * 2);
        panel_wait();
        panel_flush(0, y + row, OS_W - 1, y + row + n - 1, bounce);
        panel_wait();
    }
}

static void blit() { blit_rows(0, OS_H); }

}  // namespace surface


// ════════════════════════════════════════════════════════════
// DRAW — primitives de dessin, famille UNIQUE
// ════════════════════════════════════════════════════════════

namespace draw {

static void fill_bg() { gfx::fill(surface::cv, C_BG); }

static void wipe_rows(int y, int h) {
    gfx::fill_rect(surface::cv, 0, y, OS_W, h, C_BG);
}

// Filet de séparation — même rôle que draw::hline des pages SysInfo.
static void rule(int y, uint16_t c) {
    gfx::hline(surface::cv, OS_MARGIN, y, OS_W - OS_MARGIN * 2, c);
}

// Texte centré. size 1 = 6x8, 2 = 12x16, 3 = 18x24.
static void centered(int y, const char* s, uint16_t c, uint8_t size) {
    gfx::text(surface::cv, (OS_W - gfx::text_w(s, size)) / 2, y, s, c, C_BG, size);
}

// Pastille pleine centrée — même rôle que draw::badge des pages SysInfo.
static void badge(int y, const char* s, uint16_t bg, uint16_t fg) {
    const int w = gfx::text_w(s) + 12;
    const int x = (OS_W - w) / 2;
    gfx::fill_rect(surface::cv, x, y, w, 12, bg);
    gfx::text(surface::cv, x + 6, y + 2, s, fg, bg);
}

// L'anneau ET son pourcentage : la valeur-vedette de l'écran, l'équivalent du
// draw::big + jauge des pages SysInfo.
static void ring(int percent, uint16_t c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);

    gfx::ring(surface::cv, RING_CX, RING_CY, RING_RO, RING_RI,
              percent / 100.0f, c, C_DKCYAN);
    centered(RING_CY - (gfx::CHAR_H * 3) / 2, buf, C_WHITE, 3);
}

}  // namespace draw


// ════════════════════════════════════════════════════════════
// ÉCRAN — PROGRESSION
//   FIXE   : filets, titre, pastille d'avertissement
//   BLITTÉ : l'anneau et son pourcentage
// ════════════════════════════════════════════════════════════

static bool _decor_done = false;
static int  _last_pct   = -1;

static void draw_static() {
    draw::fill_bg();
    draw::rule(RULE_TOP_Y, C_DKCYAN);
    draw::centered(TITLE_Y, "MISE A JOUR OTA", C_CYAN, 2);
    draw::badge(BADGE_Y, "NE PAS ETEINDRE", C_RED, C_WHITE);
    draw::rule(RULE_BOT_Y, C_DKCYAN);
}

static void _screen_progress(int percent) {
    if (!surface::ensure()) return;
    percent = constrain(percent, 0, 100);

    if (!_decor_done) {
        _decor_done = true;
        _last_pct   = -1;
        draw_static();
        draw::ring(0, C_GREEN);
        surface::blit();
    }

    // ⚠️ onProgress est appelé à chaque bloc TCP — des centaines de fois pour
    // 100 valeurs. Sans ce filtre, l'anneau est redessiné en pure perte.
    if (percent == _last_pct) return;
    _last_pct = percent;

    draw::wipe_rows(RING_TOP, RING_H);
    draw::ring(percent, C_GREEN);
    surface::blit_rows(RING_TOP, RING_H);

    if (percent >= 100) {
        draw::wipe_rows(BADGE_Y, 12);
        draw::centered(BADGE_Y + 2, "Redemarrage en cours...", C_GREEN, 1);
        surface::blit_rows(BADGE_Y, 12);

        _decor_done = false;
        surface::release();
    }
}


// ════════════════════════════════════════════════════════════
// ÉCRAN — ÉCHEC
//   FIXE   : tout
//   BLITTÉ : néant
// ════════════════════════════════════════════════════════════

static void _screen_error(const char* reason) {
    if (!surface::ensure()) return;
    _decor_done = false;
    _last_pct   = -1;

    draw::fill_bg();
    draw::rule(RULE_TOP_Y, C_RED);
    draw::centered(66,  "ECHEC OTA", C_RED, 2);
    draw::centered(104, reason, C_WHITE, 1);
    draw::centered(126, "firmware precedent conserve", C_DIM, 1);
    draw::badge(170, "NOUVEL ESSAI OU REDEMARRAGE", C_WHITE, C_RED);
    draw::rule(RULE_BOT_Y, C_RED);
    surface::blit();

    surface::release();
}


// ════════════════════════════════════════════════════════════
// OTA — tâche, callbacks, API publiques
// ════════════════════════════════════════════════════════════

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
        log_line("[OTA] Début mise à jour : %s", type.c_str());
        led_pause();
        display_pause();
        wakeword_ota_suspend();   // ESP_SR vole sinon le cœur 0 à l'ota_task (prio 5 > 2)
        mqtt_ota_suspend();       // le broker sature sinon l'airtime WiFi (OTA ~2min30 → ~20s)
        audio_click();
        _screen_progress(0);
        _ota_last_pct = 0;
    });

    ArduinoOTA.onEnd([]() {
        log_line("[OTA] Terminé — redémarrage...");
        _screen_progress(100);
        delay(2000);
        ESP.restart();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int pct = (progress * 100) / total;
        if (pct == _ota_last_pct) return;   // ne rien faire tant que le % n'a pas bougé
        _ota_last_pct = pct;
        log_line("[OTA] Progression : %d%%", pct);
        _screen_progress(pct);
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
        _screen_error(desc);
        audio_click();
        delay(5000);
        led_resume();
        display_resume();
        wakeword_ota_resume();   // échec : on relâche le verrou (le succès reboote)
        mqtt_ota_resume();
    });

    ArduinoOTA.begin();

    // Tâche dédiée sur le cœur 0, priorité 2 (au-dessus de loop).
    xTaskCreatePinnedToCore(_ota_task, "ota_task", STACK_BYTES_OTA_TASK, nullptr, 2, nullptr, 0);

    log_line("[OTA] Prêt — hostname : %s.local", WIFI_HOSTNAME);
}

void ota_loop() {
    // Vide — le handle tourne sur la tâche dédiée.
}
