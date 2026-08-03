#pragma once

// ============================================================
// LED_MANAGER.H — WS2812B IO42 — effet rainbow
// ============================================================

// ---- API PUBLIQUES ----
void led_init();
void led_loop();  // à appeler dans loop()

// Coupe/reprend le rafraîchissement RMT (WS2812B), à utiliser autour de toute
// écriture flash (OTA). ⚠️ Une ISR RMT déclenchée pendant que l'IDF désactive
// le cache flash fait paniquer la puce si son code n'est pas en IRAM.
void led_pause();
void led_resume();
