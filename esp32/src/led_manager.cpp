// ============================================================
// LED_MANAGER.CPP — WS2812B IO42
// États :
//   WiFi DOWN  → rouge fixe
//   MQTT DOWN  → orange fixe
//   Audio rec  → bleu fixe
//   Tout OK    → arc-en-ciel
// ============================================================

// ---- BIBLIOTHÈQUES ----
#define FASTLED_FORCE_RMT   // doit précéder l'include — macro de config FastLED
#include <FastLED.h>

// ---- RESSOURCES LOCALES ----
#include "led_manager.h"
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "audio_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
#define RAINBOW_SPEED_MS  100    // 10 im/s : anime 1 LED sans charger loopTask
#define RAINBOW_HUE_STEP   2
#define BRIGHTNESS_NORMAL 40
#define BRIGHTNESS_ALERT  200

static CRGB           _leds[LED_RGB_NUM];
static uint8_t        _hue = 0;
static unsigned long  _last_update = 0;
static bool           _paused = false;

// ---- API PUBLIQUES ----

void led_init() {
    FastLED.addLeds<WS2812B, LED_RGB_PIN, GRB>(_leds, LED_RGB_NUM);
    FastLED.setBrightness(BRIGHTNESS_NORMAL);
    _leds[0] = CHSV(0, 255, 255);
    FastLED.show();
    log_line("[LED] WS2812B init OK");
}

void led_loop() {
    if (_paused) return;   // pas de FastLED.show() pendant une écriture flash (OTA)

    unsigned long now = millis();
    if (now - _last_update < RAINBOW_SPEED_MS) return;
    _last_update = now;

    if (!wifi_is_connected()) {
        // Rouge fixe — WiFi down
        FastLED.setBrightness(BRIGHTNESS_ALERT);
        _leds[0] = CRGB::Red;
    } else if (!mqtt_is_connected()) {
        // Orange fixe — MQTT down
        FastLED.setBrightness(BRIGHTNESS_ALERT);
        _leds[0] = CRGB(255, 80, 0);
    } else if (audio_is_recording) {
        // Bleu fixe — enregistrement audio
        FastLED.setBrightness(BRIGHTNESS_ALERT);
        _leds[0] = CRGB(0, 0, 255);
    } else {
        // Arc-en-ciel — tout OK
        FastLED.setBrightness(BRIGHTNESS_NORMAL);
        _leds[0] = CHSV(_hue, 255, 255);
        _hue += RAINBOW_HUE_STEP;
    }

    FastLED.show();
}

// À appeler juste avant une opération sensible au cache flash désactivé
// (OTA notamment) : bloque tout appel à FastLED.show() donc toute
// activité RMT tant que la pause est active. Ne touche pas à l'état
// actuel de la LED (elle reste figée sur sa dernière valeur affichée).
void led_pause() {
    _paused = true;
}

void led_resume() {
    _paused = false;
    _last_update = millis();   // évite un premier rafraîchissement instantané "en retard"
}
