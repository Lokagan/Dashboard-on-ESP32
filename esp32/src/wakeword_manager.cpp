// ============================================================
// WAKEWORD_MANAGER.CPP — Détection du mot-clé "Jarvis" (ESP_SR / WakeNet)
//
// ESP_SR lance SA propre tâche de détection et appelle notre callback depuis
// elle : le callback lève un drapeau, drainé par wakeword_loop() sur loopTask.
// Bus I2S partagé avec audio_manager, qui encadre ses opérations par
// wakeword_pause()/wakeword_resume().
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <ESP_SR.h>
#include <esp_heap_caps.h>

// ---- RESSOURCES LOCALES ----
#include "wakeword_manager.h"
#include "audio_manager.h"
#include "ai_manager.h"
#include "display_manager.h"
#include "light_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
// Vrai une fois ESP_SR.begin() réussi — garde les pause/resume appelés trop tôt.
static bool _sr_started = false;

// Verrou OTA : maintient ESP_SR en pause pendant tout le flash.
static bool _ota_suspended = false;

// Interne consommé par ESP_SR (delta autour de begin), lu par sysinfo_manager.
static uint32_t _esp_sr_internal_bytes = 0;

// ---- HELPERS ----

// Écrit par la tâche ESP_SR, lu par loopTask.
static volatile bool _trigger_pending = false;

// Appelé depuis la tâche ESP_SR : lever le drapeau, rien d'autre (ni LVGL, ni état IA).
static void _on_sr_event(sr_event_t event, int command_id, int phrase_id) {
    if (event != SR_EVENT_WAKEWORD) return;   // wake word seul (pas de commandes MultiNet)
    log_line("[Wakeword] \"Jarvis\" détecté");
    _trigger_pending = true;
}

// ---- API PUBLIQUES ----
void wakeword_init() {
    // Marge d'interne avant ESP_SR, et mesure de ce qu'il consomme.
    uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    log_line("[Wakeword] RAM interne libre avant ESP_SR : %u o (plus gros bloc : %u o)",
             (unsigned)freeBefore,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    ESP_SR.onEvent(_on_sr_event);

    // Bus I2S partagé, déjà démarré par audio_init(). Défauts ESP_SR : stéréo
    // (micro sur le canal gauche), wake word seul, modèle lu en partition "model".
    if (!ESP_SR.begin(audio_get_i2s(), NULL, 0)) {
        log_line("[Wakeword] ESP_SR.begin a échoué — partition 'model' vide ? modèle absent ?");
        return;
    }
    _sr_started = true;
    _esp_sr_internal_bytes = freeBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    log_line("[Wakeword] ESP_SR démarré (écoute active, mot-clé Jarvis) — %u o d'interne",
             (unsigned)_esp_sr_internal_bytes);
}

uint32_t wakeword_esp_sr_internal_bytes() {
    return _esp_sr_internal_bytes;
}

// Appelée depuis loop(), donc sur loopTask — seul thread autorisé à toucher LVGL
// et l'état IA. Déclenche sauf si listening/thinking/speaking : AI_ERROR compris,
// on doit pouvoir en sortir à la voix.
void wakeword_loop() {
    if (!_trigger_pending) return;
    _trigger_pending = false;

    AiState s = ai_get_state();
    if (s != AI_LISTENING && s != AI_THINKING && s != AI_SPEAKING) {
        // ⚠️ Explicite malgré le réveil sur changement d'écran de light_loop() :
        // si la veille est tombée alors que l'écran IA était DÉJÀ affiché,
        // lv_scr_act() ne change pas et rien ne réveillerait la dalle.
        light_wake();
        display_show_ai_auto();   // vocal : arme le retour auto à l'écran précédent
        audio_wakeword_ack();
        ai_start_listening();
    } else {
        log_line("[Wakeword] Ignore — IA déjà occupée");
    }
}

// ⚠️ DOUBLE appel volontaire, contournement d'une course DANS ESP_SR — ne pas
// « simplifier ». audio_feed_task attend PAUSE_FEED *et* RESUME_FEED puis
// efface les deux (esp32-hal-sr.c). Si resume() tombe alors que la tâche n'est
// pas encore bloquée — elle était dans sa lecture I2S —, RESUME_FEED reste
// posé ; la pause suivante trouve les deux bits et traverse l'attente sans
// s'arrêter. ESP_SR continue alors de lire le micro PENDANT l'enregistrement,
// les deux lecteurs se partagent l'anneau DMA et la capture perd les deux
// tiers de ses échantillons (mesuré : x2,9 de temps réel pour l'audio produit,
// phrase déchiquetée). Le 1er appel consomme le bit resté posé, le 2e pause
// réellement ; le délai laisse la tâche atteindre son test.
#define SR_PAUSE_SETTLE_MS 40   // > un chunk AFE (~32 ms)

void wakeword_pause() {
    if (!_sr_started) return;
    ESP_SR.pause();
    vTaskDelay(pdMS_TO_TICKS(SR_PAUSE_SETTLE_MS));
    ESP_SR.pause();
}

void wakeword_resume() {
    // Respecte le verrou OTA : _audio_task émet un resume après le click de début d'OTA.
    if (_sr_started && !_ota_suspended) ESP_SR.resume();
}

// Suspension DURE réservée à l'OTA, malgré les pause/resume de _audio_task.
void wakeword_ota_suspend() { _ota_suspended = true;  if (_sr_started) ESP_SR.pause();  }
void wakeword_ota_resume()  { _ota_suspended = false; if (_sr_started) ESP_SR.resume(); }
