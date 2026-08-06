// ============================================================
// MAIN.CPP — point d'entrée.
// Initialise les briques et les fait tourner. Aucune logique métier
// ici, tout est dans les managers.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "log_manager.h"
#include "littlefs_manager.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "display_manager.h"
#include "touch_manager.h"
#include "light_manager.h"
#include "led_manager.h"
#include "audio_manager.h"
#include "ota_manager.h"
#include "ai_manager.h"
#include "ai_companion.h"
#include "http_manager.h"
#include "wakeword_manager.h"

// Pile de loopTask (STACK_BYTES_LOOP_TASK).
// ⚠️ À portée globale : la macro définit un symbole lu au démarrage.
SET_LOOP_TASK_STACK_SIZE(STACK_BYTES_LOOP_TASK);

// ---- OBJETS GLOBAUX ----

// Chronométrage des managers : tous partagent loopTask, celui qui traîne
// retarde TOUS les autres.
#define LOOP_SLOW_MS    20
#define LOOP_REPORT_MS  10000

// Postes chronométrés. L'ordre est celui de la ligne de synthèse.
enum { L_WIFI, L_MQTT, L_HTTP, L_LVGL, L_LED, L_LIGHT, L_AI, L_WW, L_COUNT };
static const char* const _loop_names[L_COUNT] =
    { "wifi", "mqtt", "http", "lvgl", "led", "light", "ai", "ww" };

static uint32_t      _loop_us[L_COUNT];   // cumul sur la fenêtre
static uint32_t      _loop_turns;         // tours de boucle sur la fenêtre
static unsigned long _loop_t0;            // début de la fenêtre

// La MESURE tourne toujours ; seule la SORTIE est conditionnée — sinon
// réactiver donnerait une fenêtre tronquée.
#define TIMED(idx, call) do {                                               \
        uint32_t _t0 = micros();                                            \
        call;                                                               \
        uint32_t _dt = micros() - _t0;                                      \
        _loop_us[idx] += _dt;                                               \
        if (_dt > LOOP_SLOW_MS * 1000UL && log_loop_measure())              \
            log_line("[LOOP] slow %s : %lums (>%ims)", _loop_names[idx],    \
                     (unsigned long)(_dt / 1000), LOOP_SLOW_MS);            \
    } while (0)

// Variante muette, pour les managers qui signalent eux-mêmes leurs lenteurs.
#define TIMED_ACC(idx, call) do {                                    \
        uint32_t _t0 = micros();                                     \
        call;                                                        \
        _loop_us[idx] += micros() - _t0;                             \
    } while (0)

// Le %CPU par cœur de SysInfo ne dit RIEN du core 1 (loopTask y affame IDLE1,
// qui lit donc 100 % en permanence) : seul le temps passé ici le dit.
static void _loop_report() {
    unsigned long now = millis();
    if (now - _loop_t0 < LOOP_REPORT_MS) return;

    // Fenêtre roulée dans tous les cas : mesure coupée, seule la ligne disparaît.
    if (log_loop_measure()) {
        uint32_t total = 0;
        for (int i = 0; i < L_COUNT; i++) total += _loop_us[i];

        // Le journal tronque à LOG_LINE_LEN : on n'écrit que les postes > 1 ms.
        char line[LOG_LINE_LEN];
        int  n = snprintf(line, sizeof(line), "[LOOP] %lu%% %lut",
                          (unsigned long)(total / (LOOP_REPORT_MS * 10)),
                          (unsigned long)_loop_turns);
        for (int i = 0; i < L_COUNT && n > 0 && n < (int)sizeof(line); i++)
            if (_loop_us[i] >= 1000)
                n += snprintf(line + n, sizeof(line) - n, " %s:%lu",
                              _loop_names[i], (unsigned long)(_loop_us[i] / 1000));
        log_line("%s", line);
    }

    memset(_loop_us, 0, sizeof(_loop_us));
    _loop_turns = 0;
    _loop_t0    = now;
}

// ---- SÉQUENCE D'INITIALISATION ----

void setup() {
    Serial.begin(115200);

    // Écritures série NON BLOQUANTES : sur USB CDC, write() attend que l'hôte
    // vide son tampon (250 ms), et log_line() tourne sur loopTask.
    Serial.setTxTimeoutMs(0);

    delay(2000); // laisse l'énumération USB se faire avant les premiers logs
    log_init();  // journal circulaire — logge aussi la raison du reset
    log_line("[BOOT] Dashboard ESP32-S3 starting...");

    // L'ordre compte : écran d'abord (feedback visuel), réseau ensuite.
    display_init();
    touch_init();
    light_init();         // APRÈS touch_init : bus Wire
    littlefs_init();      // audio, HTTP, sysinfo, avatar Companion
    wifi_connect();       // non bloquant, la connexion se termine via wifi_loop()
    ota_init();
    mqtt_init();
    http_init();
    led_init();
    audio_init();         // démarre le bus I2S partagé
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
    ai_init();
    ai_companion_init();
    wakeword_init();      // APRÈS audio_init (bus I2S) et ai_init
    log_line("[BOOT] All systems ready.");
    display_show_home();
    _loop_t0 = millis();   // la 1re fenêtre ne doit pas compter le boot
}

// ---- API PUBLIQUES ----

void loop() {
    TIMED(L_WIFI, wifi_loop());
    TIMED(L_MQTT, mqtt_loop());   // draine la file esp-mqtt → applique les display_update_*
    TIMED(L_HTTP, http_loop());
    TIMED_ACC(L_LVGL, display_loop());   // se logge lui-même ("[LVGL] slow")
    TIMED(L_LED, led_loop());
    TIMED(L_LIGHT, light_loop());
    TIMED(L_AI,  ai_loop());
    // Doit être sur loopTask : le callback d'ESP_SR ne lève qu'un drapeau.
    TIMED(L_WW, wakeword_loop());
    // Vides, conservées pour la symétrie du pattern manager : audio et OTA
    // tournent sur leur propre tâche, LVGL fait le polling tactile.
    touch_loop();
    audio_loop();
    ota_loop();
    _loop_turns++;
    _loop_report();
    // Rend la main à IDLE1 : la boucle Arduino ne cède JAMAIS sur dual-core
    // (yieldIfNecessary() est sous #if CONFIG_FREERTOS_UNICORE, non défini).
    // 1 tick = 1 ms (FREERTOS_HZ).
    vTaskDelay(1);
}
