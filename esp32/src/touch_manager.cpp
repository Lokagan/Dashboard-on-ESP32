// ============================================================
// TOUCH_MANAGER.CPP — pilote tactile FT6336G (I2C) : lecture des
// registres et injection des événements dans le driver d'entrée LVGL.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "touch_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
// Registres FT6336G
#define FT6336_ADDR         TOUCH_ADDR
#define FT6336_REG_NUMTCH   0x02
#define FT6336_REG_P1_XH    0x03
#define FT6336_REG_P1_XL    0x04
#define FT6336_REG_P1_YH    0x05
#define FT6336_REG_P1_YL    0x06

#define FT6336_TIMEOUT_MS      5
#define TOUCH_TIMEOUT_LOG_MS   1000   // un message par seconde au plus

static lv_indev_t* _indev       = nullptr;
static bool        _was_touched = false;
static lv_point_t  _last_point  = {0, 0};

// Un appui qui ne se relâche jamais serait SILENCIEUX : le log de point n'est
// émis qu'au relâchement.
static uint32_t      _pressed_reads    = 0;
static unsigned long _last_timeout_log = 0;

// ---- HELPERS ----

static uint8_t _ft6336_read(uint8_t reg) {
    Wire.beginTransmission(FT6336_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;   // erreur bus
    Wire.requestFrom((uint8_t)FT6336_ADDR, (uint8_t)1);

    unsigned long t = millis();
    while (!Wire.available()) {
        if (millis() - t > FT6336_TIMEOUT_MS) {
            // Throttlé, et surtout HORS de la boucle : des centaines de lignes
            // (donc de Serial.println, sur loopTask) pour une lecture lente.
            unsigned long now = millis();
            if (now - _last_timeout_log > TOUCH_TIMEOUT_LOG_MS) {
                _last_timeout_log = now;
                log_line("[Touch] timeout I2C sur reg 0x%02X", reg);
            }
            return 0;
        }
    }
    return Wire.read();
}

// ---- API LOCALES ----

static void _lv_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint8_t num_touches = _ft6336_read(FT6336_REG_NUMTCH);

    if (num_touches > 0 && num_touches < 6) {
        uint8_t xh = _ft6336_read(FT6336_REG_P1_XH);
        uint8_t xl = _ft6336_read(FT6336_REG_P1_XL);
        uint8_t yh = _ft6336_read(FT6336_REG_P1_YH);
        uint8_t yl = _ft6336_read(FT6336_REG_P1_YL);

        int raw_x = ((xh & 0x0F) << 8) | xl;
        int raw_y = ((yh & 0x0F) << 8) | yl;

        // Rotation 1 (paysage)
        data->point.x = raw_y;
        data->point.y = SCREEN_HEIGHT - raw_x;
        data->state   = LV_INDEV_STATE_PRESSED;

        _last_point  = data->point;
        _was_touched = true;

        // ~50 lectures ≈ 1,5 s, plus long qu'un appui humain. Journalisé une
        // seule fois par appui (== et non >=).
        if (++_pressed_reads == 50) {
            log_line("[Touch] Appui continu (50 lectures) — x=%d y=%d, num=%d, raw=%d/%d",
                     data->point.x, data->point.y, (int)num_touches, raw_x, raw_y);
        }
    } else {
        if (_was_touched) {
            log_line("[Touch] Point : x=%d, y=%d (apres %lu lectures)",
                     _last_point.x, _last_point.y, (unsigned long)_pressed_reads);
            _was_touched = false;
        }
        _pressed_reads = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- API PUBLIQUES ----

void touch_init() {
    log_line("[Touch] Init FT6336G...");

    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(100);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);   // 400 kHz MAX — au-delà le contrôleur se bloque

    Wire.beginTransmission(FT6336_ADDR);
    if (Wire.endTransmission() != 0) {
        log_line("[Touch] FT6336G non détecté !");
        return;
    }
    log_line("[Touch] FT6336G détecté OK");

    _indev = lv_indev_create();
    lv_indev_set_type(_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(_indev, _lv_touch_read_cb);

    log_line("[Touch] Driver LVGL enregistré");
}

// Vide : LVGL fait le polling via lv_timer_handler() dans display_loop().
void touch_loop() {}
