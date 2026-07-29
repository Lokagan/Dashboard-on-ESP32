// ============================================================
// WAKEWORD_MANAGER.CPP — Détection du mot-clé "Jarvis" (ESP_SR / WakeNet)
//
// Wrapper Arduino officiel ESP_SR (core 3.x). ESP_SR.begin() reçoit le bus I2S
// partagé (audio_get_i2s()) et lance SA propre tâche de détection, qui lit le
// micro en continu. Sur wake word, ESP_SR appelle notre callback depuis cette
// tâche interne — on ne peut donc RIEN y faire qui touche LVGL ou l'état IA. Le
// callback se contente de lever un drapeau, drainé par wakeword_loop() depuis
// loop() (même règle que ai_manager.cpp::_post_state_async).
//
// Accès exclusif au bus : audio_manager encadre toute opération I2S par
// wakeword_pause()/wakeword_resume() (cf. _audio_task) — ESP_SR n'écoute donc
// que quand audio_manager est au repos, jamais deux lecteurs I2S en même temps.
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
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
// Vrai une fois ESP_SR.begin() réussi — garde les pause/resume appelés tôt
// (audio_manager peut jouer un son avant wakeword_init()).
static bool _sr_started = false;

// Interne consommé par ESP_SR (delta libre avant/après begin), pour le bilan
// mémoire de sysinfo_manager. 0 tant que begin() n'a pas réussi.
static uint32_t _esp_sr_internal_bytes = 0;

// ---- HELPERS ----

// Détection en attente de traitement. Simple drapeau : il n'y a aucune donnée à
// transporter, et une détection de plus pendant qu'une autre attend n'apporte
// rien. `volatile` car écrit par la tâche ESP_SR et lu par loopTask.
static volatile bool _trigger_pending = false;

// Callback ESP_SR — appelé depuis SA tâche interne, PAS depuis loopTask.
// On n'y fait donc RIEN d'autre que lever le drapeau : ni LVGL, ni état IA.
// (L'ancienne version appelait lv_async_call() ici — cause racine de panics,
//  cf. le commentaire détaillé dans ai_manager.cpp::_post_state_async.)
static void _on_sr_event(sr_event_t event, int command_id, int phrase_id) {
    if (event != SR_EVENT_WAKEWORD) return;   // wake word seul (pas de commandes MultiNet)
    log_line("[Wakeword] \"Jarvis\" détecté");
    _trigger_pending = true;
}

// ---- API PUBLIQUES ----
void wakeword_init() {
    // Diagnostic RAM interne — les tâches d'ESP_SR (feed/detect/handler + AFE)
    // s'allouent en RAM interne ; on veut voir la marge avant begin() ET mesurer
    // ce qu'ESP_SR consomme (delta avant/après, pour le bilan mémoire de SysInfo).
    uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    log_line("[Wakeword] RAM interne libre avant ESP_SR : %u o (plus gros bloc : %u o)",
             (unsigned)freeBefore,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    ESP_SR.onEvent(_on_sr_event);

    // Bus I2S partagé, déjà démarré par audio_init(). Défauts ESP_SR : rx_chan
    // = SR_CHANNELS_STEREO (notre I2S est stéréo, micro sur le canal gauche),
    // mode = SR_MODE_WAKEWORD, input_format = "MN" (canal 0 = micro, 1 = inutilisé).
    // Modèle "jarvis" lu depuis la partition flash "model" (esp_srmodel_init en interne).
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

// Traite une détection en attente. Appelée depuis loop() (main.cpp), donc sur
// loopTask — seul thread autorisé à toucher LVGL et l'état IA.
//
// Même condition que le bouton tactile (display_manager.cpp) : déclenche sauf si
// listening/thinking/speaking (donc y compris AI_ERROR, dont on doit pouvoir
// sortir à la voix). Séquence écran Companion -> jingle -> écoute : les deux
// commandes audio sont des dépôts FIFO dans _audio_task, le jingle finit avant
// que l'enregistrement démarre, sans callback de fin de lecture à gérer.
void wakeword_loop() {
    if (!_trigger_pending) return;
    _trigger_pending = false;

    AiState s = ai_get_state();
    if (s != AI_LISTENING && s != AI_THINKING && s != AI_SPEAKING) {
        display_show_ai_auto();   // vocal : arme le retour auto à l'écran précédent
        audio_wakeword_ack();
        ai_start_listening();
    } else {
        log_line("[Wakeword] Ignore — IA deja occupee");
    }
}

void wakeword_pause() {
    if (_sr_started) ESP_SR.pause();
}

void wakeword_resume() {
    if (_sr_started) ESP_SR.resume();
}
