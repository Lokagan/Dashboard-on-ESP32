#pragma once

// ============================================================
// LED_MANAGER.H — WS2812B IO42 — effet rainbow
// ============================================================

// ---- API PUBLIQUES ----
void led_init();
void led_loop();  // à appeler dans loop()

// Coupe/reprend le rafraîchissement RMT (WS2812B). À utiliser autour de
// toute écriture flash sensible au cache désactivé (OTA notamment) —
// une interruption RMT qui se déclenche pendant que l'ESP-IDF désactive
// le cache flash pour écrire un secteur peut faire paniquer la puce si
// le code de l'ISR n'est pas en IRAM (cas classique FastLED + OTA).
void led_pause();
void led_resume();
