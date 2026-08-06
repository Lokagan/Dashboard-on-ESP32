#pragma once

// ============================================================
// LIGHT_MANAGER.H — APDS-9930 (I2C) : lumière ambiante et proximité.
// Asservit le rétroéclairage, met l'écran en veille sur absence et
// déclenche Jarvis sur un passage de main.
//
// Le capteur est HOTPLUGGABLE : absent au boot il est re-sondé toutes les
// LIGHT_PROBE_MS, et sa disparition en cours de route est détectée puis
// rattrapée sans redémarrage.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <stdint.h>

// ---- OBJETS GLOBAUX ----

// Instantané pour la page CAPTEUR de SysInfo. Rempli sous le seul point de
// vérité du manager — aucun consommateur ne relit le capteur lui-même.
struct LightStatus {
    bool     present;
    uint16_t prox;
    uint16_t prox_base;     // repos suivi en continu
    uint16_t thr_near, thr_far;   // seuils vivants, base + delta
    uint32_t clux;          // CENTILUX (lux x100) — ce montage vit sous 1 lux
    uint16_t ch0, ch1;      // canaux bruts (visible + IR)
    bool     near;          // main actuellement au-dessus du seuil PROCHE
    bool     auto_on;
    bool     manual_hold;   // auto suspendu par un réglage manuel
    bool     asleep;
    int      brightness;    // dernière consigne posée par l'asservissement
    uint32_t gestures;      // compteur depuis le boot
    uint32_t since_gesture; // ms depuis le dernier geste (0 si aucun)
    uint32_t since_present; // ms depuis la dernière présence devant le capteur
    uint32_t plugs;         // nombre de détections du capteur (hotplug)
};

// ---- API PUBLIQUES ----

void light_init();   // APRÈS touch_init() : c'est lui qui démarre le bus Wire
void light_loop();   // depuis loop() — thread principal

void light_set_auto(bool on);
bool light_is_auto();

// Un réglage manuel (SliderLCD, brightness:N) réveille l'écran et suspend
// l'asservissement pendant LIGHT_MANUAL_HOLD_MS.
void light_notify_manual();

// Sortie de veille sans rien déclencher d'autre — le wake word « Jarvis » doit
// rallumer l'écran comme le ferait un passage de main.
void light_wake();

// Appelé par le tactile au PREMIER appui d'une séquence. Renvoie true si cet
// appui a servi à sortir de veille : il ne doit alors pas atteindre LVGL.
bool light_touch_wake();

void light_get_status(LightStatus* out);
void light_log_state();   // cmd "light" — lux, proximité, mode, veille
