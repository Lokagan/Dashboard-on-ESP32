// ============================================================
// LIGHT_MANAGER.CPP — APDS-9930 (0x39) sur le bus I2C du tactile.
//
// ⚠️ L'APDS-9930 n'a PAS de moteur de gestes (c'est l'APDS-9960) : le
// "geste" est un passage de main reconstruit ici depuis le seul canal de
// proximité.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <math.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "light_manager.h"
#include "display_manager.h"
#include "sysinfo_manager.h"
#include "audio_manager.h"
#include "ai_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----

// Registres APDS-9930. ⚠️ Toute adresse s'émet via APDS_CMD_AUTO : le bit 7
// marque l'octet de commande, les bits 6:5 le mode d'adressage.
#define APDS_CMD_AUTO   0xA0   // commande + auto-incrément
#define APDS_ENABLE     0x00
#define APDS_ATIME      0x01
#define APDS_PTIME      0x02
#define APDS_WTIME      0x03
#define APDS_CONFIG     0x0D
#define APDS_PPULSE     0x0E
#define APDS_CONTROL    0x0F
#define APDS_ID         0x12
#define APDS_CH0DATA    0x14   // Ch0 puis Ch1 : 4 octets consécutifs
#define APDS_PDATA      0x18

#define APDS_I2C_TIMEOUT_MS   5

// Coefficients de conversion en lux (datasheet APDS-9930, capteur à l'air
// libre). LUX_ATIME_MS suit APDS_ATIME : 2,73 ms x (256 - ATIME).
#define LUX_GA        0.49f
#define LUX_DF        52.0f
#define LUX_B         1.862f
#define LUX_C         0.746f
#define LUX_D         1.291f
#define LUX_ATIME_MS  101.0f

// Gain ALS. ⚠️ Les deux constantes vont ENSEMBLE : APDS_AGAIN_BITS part dans
// CONTROL, LUX_AGAIN dans le calcul. Les désynchroniser fausse les lux du
// rapport exact des deux.
//   bits  gain   pleine échelle à ATIME 101 ms
//   0x00    1x   ~9560 lux
//   0x01    8x   ~1195 lux
//   0x02   16x    ~600 lux
//   0x03  120x     ~80 lux
#define APDS_AGAIN_BITS  0x02
#define LUX_AGAIN        16.0f

// CONTROL, assemblé pièce par pièce plutôt qu'en constante opaque.
#define APDS_PDRIVE_100MA  0x00   // courant LED max — 0x40 50 mA, 0x80 25 mA, 0xC0 12,5 mA
#define APDS_PDIODE_CH1    0x20   // seule valeur valide en proximité
#define APDS_PGAIN_BITS    0x08   // 0x00 1x | 0x04 2x | 0x08 4x | 0x0C 8x

// Impulsions IR par mesure de proximité (1-255). ⚠️ La PORTÉE ne suit pas le
// signal : le retour d'une surface diffuse décroît en 1/d⁴, donc x4
// d'impulsions ne vaut que x1,4 de distance.
#define APDS_PPULSE_COUNT  32

// --- Présence du capteur (hotplug) ---
static bool     _present     = false;
static uint8_t  _strikes     = 0;   // lectures ratées consécutives
static uint32_t _last_probe  = 0;
static uint32_t _plugs       = 0;

// --- Luminosité ---
static bool     _auto_on     = true;
static bool     _manual_hold = false;
static uint32_t _manual_t0   = 0;
static bool     _force_apply = true;   // ignore l'hystérésis au prochain calcul
static float    _clux_avg    = -1.0f;
static int      _applied_pct = DISPLAY_BRIGHTNESS_DEFAULT;

// --- Veille ---
static bool       _asleep       = false;
static uint32_t   _last_present = 0;
static lv_obj_t*  _last_screen  = nullptr;   // détection de changement d'écran

// --- Geste ---
static float    _prox_base    = -1.0f;   // repos suivi en continu (IR ambiant)
static uint16_t _thr_near     = 0;       // recalculés à chaque poll depuis la base
static uint16_t _thr_far      = 0;
static bool     _ceiling_warned = false;
static bool     _near         = false;
static bool     _near_woke    = false;   // ce passage n'a servi qu'à réveiller
static uint8_t  _persist      = 0;       // échantillons confirmant une transition
static uint16_t _prox_hist[3] = {0, 0, 0};
static uint8_t  _prox_n       = 0;
static uint32_t _near_since   = 0;
static uint32_t _last_gesture = 0;
static uint32_t _gestures     = 0;

static uint32_t _last_poll     = 0;
static uint8_t  _als_countdown = 0;
static uint16_t _last_prox     = 0;
static uint32_t _last_clux     = 0;
static uint16_t _last_ch0      = 0;
static uint16_t _last_ch1      = 0;

// ---- HELPERS ----

static bool _apds_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(APDS_ADDR);
    Wire.write(APDS_CMD_AUTO | reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool _apds_read(uint8_t reg, uint8_t* dst, uint8_t len) {
    Wire.beginTransmission(APDS_ADDR);
    Wire.write(APDS_CMD_AUTO | reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)APDS_ADDR, len);

    unsigned long t = millis();
    for (uint8_t i = 0; i < len; i++) {
        while (!Wire.available())
            if (millis() - t > APDS_I2C_TIMEOUT_MS) return false;
        dst[i] = Wire.read();
    }
    return true;
}

// Deux estimations concurrentes, la plus grande gagne (datasheet).
// ⚠️ Rendu en CENTILUX : en lux entiers, tout ce montage retombait à 0.
static uint32_t _clux_from(uint16_t ch0, uint16_t ch1) {
    float iac1 = (float)ch0 - LUX_B * (float)ch1;
    float iac2 = LUX_C * (float)ch0 - LUX_D * (float)ch1;
    float iac  = fmaxf(fmaxf(iac1, iac2), 0.0f);
    float lpc  = (LUX_GA * LUX_DF) / (LUX_ATIME_MS * LUX_AGAIN);
    return (uint32_t)(iac * lpc * 100.0f);
}

// Racine carrée : l'œil discrimine bien plus finement en bas d'échelle.
static int _pct_from_clux(uint32_t clux) {
    if (clux <= LIGHT_CLUX_DARK)   return LIGHT_BRIGHTNESS_MIN;
    if (clux >= LIGHT_CLUX_BRIGHT) return LIGHT_BRIGHTNESS_MAX;
    float r = (float)(clux - LIGHT_CLUX_DARK) /
              (float)(LIGHT_CLUX_BRIGHT - LIGHT_CLUX_DARK);
    return LIGHT_BRIGHTNESS_MIN +
           (int)(sqrtf(r) * (LIGHT_BRIGHTNESS_MAX - LIGHT_BRIGHTNESS_MIN) + 0.5f);
}

// Médiane glissante sur 3 : supprime le pic ISOLÉ, qui est la forme que prend
// ici le parasite. Une moyenne, elle, le diluerait sans l'éliminer.
static uint16_t _prox_median(uint16_t raw) {
    _prox_hist[2] = _prox_hist[1];
    _prox_hist[1] = _prox_hist[0];
    _prox_hist[0] = raw;
    if (_prox_n < 3) { _prox_n++; return raw; }

    uint16_t a = _prox_hist[0], b = _prox_hist[1], c = _prox_hist[2];
    return a < b ? (b < c ? b : (a < c ? c : a))
                 : (a < c ? a : (b < c ? c : b));
}

// ⚠️ Le compteur d'absence est REMIS À ZÉRO ici, pas seulement chez l'appelant :
// réveiller après le délai de veille sans le faire renverrait l'écran en veille
// au poll suivant, 100 ms plus tard.
static void _wake() {
    if (!_asleep) return;
    _asleep       = false;
    _force_apply  = true;
    _last_present = millis();
    display_backlight_sleep(false);
    log_line("[Light] Réveil");
}

// Toute NAVIGATION sort de la veille, sans un seul appel à poser chez les
// appelants : l'écran actif change quelle qu'en soit l'origine — tactile, MQTT,
// page web, wake word. Le tactile pur passe par light_touch_wake().
static void _wake_on_interaction() {
    lv_obj_t* scr = lv_scr_act();
    if (scr == _last_screen) return;
    _last_screen = scr;
    _wake();
}

// Détection + configuration complète : rejouée telle quelle à chaque
// rebranchement, le capteur repart de ses valeurs d'usine.
static bool _probe() {
    uint8_t id;
    if (!_apds_read(APDS_ID, &id, 1)) return false;

    // ENABLE remis à zéro d'abord : la configuration ne se modifie qu'à l'arrêt.
    _apds_write(APDS_ENABLE,  0x00);
    _apds_write(APDS_ATIME,   0xDB);   // cf. LUX_ATIME_MS
    _apds_write(APDS_PTIME,   0xFF);
    _apds_write(APDS_WTIME,   0xFF);
    _apds_write(APDS_PPULSE,  APDS_PPULSE_COUNT);
    _apds_write(APDS_CONFIG,  0x00);
    _apds_write(APDS_CONTROL, APDS_PDRIVE_100MA | APDS_PDIODE_CH1 |
                              APDS_PGAIN_BITS   | APDS_AGAIN_BITS);
    if (!_apds_write(APDS_ENABLE, 0x0F)) return false;   // PON | AEN | PEN | WEN

    _present      = true;
    _strikes      = 0;
    _near         = false;
    _persist      = 0;
    _prox_n       = 0;
    _prox_base    = -1.0f;
    _ceiling_warned = false;
    _clux_avg     = -1.0f;
    _force_apply  = true;
    _last_present = millis();
    _plugs++;
    log_line("[Light] APDS-9930 détecté (ID 0x%02X)", id);
    return true;
}

// Débranchement : rendre l'écran à l'utilisateur avant tout, sans quoi une
// veille en cours resterait définitive.
static void _lost() {
    _present = false;
    _near    = false;
    _wake();
    log_line("[Light] APDS-9930 perdu — sondage toutes les %d ms", LIGHT_PROBE_MS);
}

static void _gesture_trigger() {
    _gestures++;
    // Page CAPTEUR affichée : on compte le geste et on s'arrête là, c'est ce
    // qui permet de régler les seuils sans partir en écoute à chaque passage.
    if (sysinfo_sensor_page_active()) {
        log_line("[Light] Geste #%lu — page CAPTEUR, IA non sollicitée",
                 (unsigned long)_gestures);
        return;
    }

    AiState s = ai_get_state();
    if (s == AI_LISTENING || s == AI_THINKING || s == AI_SPEAKING) {
        log_line("[Light] Geste ignoré — IA déjà occupée");
        return;
    }
    log_line("[Light] Geste — écoute");
    display_show_ai_auto();
    audio_wakeword_ack();
    ai_start_listening();
}

// ---- API PUBLIQUES ----

// Une absence au boot n'est PAS un échec : light_loop() reprend le sondage.
void light_init() {
    log_line("[Light] Init APDS-9930...");
    _last_probe = millis();
    if (!_probe()) log_line("[Light] APDS-9930 absent — sondage périodique actif");
}

void light_loop() {
    uint32_t now = millis();

    if (now - _last_poll < LIGHT_POLL_MS) return;
    _last_poll = now;

    _wake_on_interaction();

    if (!_present) {
        if (now - _last_probe >= LIGHT_PROBE_MS) {
            _last_probe = now;
            _probe();
        }
        return;
    }

    uint8_t pb[2];
    if (!_apds_read(APDS_PDATA, pb, 2)) {
        // Une lecture ratée isolée est une collision de bus, pas un arrachage.
        if (++_strikes >= LIGHT_LOST_STRIKES) _lost();
        return;
    }
    _strikes = 0;
    uint16_t prox = _prox_median((uint16_t)pb[1] << 8 | pb[0]);
    _last_prox = prox;

    // Ligne de base du repos, suivie en continu. Elle n'est adaptée que HORS
    // détection, sinon une main tenue devant l'écran finirait par devenir le
    // nouveau repos et le geste ne déclencherait plus jamais.
    if (_prox_base < 0.0f) _prox_base = (float)prox;
    _thr_near = (uint16_t)(_prox_base + LIGHT_PROX_NEAR_DELTA);
    _thr_far  = (uint16_t)(_prox_base + LIGHT_PROX_FAR_DELTA);

    // ⚠️ La diaphonie occupe déjà ~60 % de l'échelle et dérive avec la
    // température : un seuil au-dessus de 1023 rendrait le geste indétectable
    // SANS AUCUNE ERREUR. Écrêté, et signalé une seule fois.
    if (_thr_near > LIGHT_PROX_CEILING) {
        _thr_near = LIGHT_PROX_CEILING;
        if (_thr_far >= _thr_near) _thr_far = _thr_near - 1;
        if (!_ceiling_warned) {
            _ceiling_warned = true;
            log_line("[Light] Diaphonie haute (repos %u) — seuils écrêtés à %d",
                     (unsigned)_prox_base, LIGHT_PROX_CEILING);
        }
    }
    if (prox < _thr_near)
        _prox_base += ((float)prox - _prox_base) / LIGHT_PROX_BASE_DIV;

    // --- Geste : franchissement du seuil PROCHE, puis retour sous LOIN dans la
    // fenêtre. Une main immobile dépasse LIGHT_GESTURE_MAX_MS et ne déclenche rien.
    // ⚠️ Toute transition demande LIGHT_PROX_PERSIST échantillons CONSÉCUTIFS.
    // Sur un seul, un parasite franchit PROCHE puis retombe sous LOIN au poll
    // suivant : la durée obtenue tombe dans la fenêtre et fabrique un geste.
    bool now_near = _near ? (prox > _thr_far) : (prox >= _thr_near);
    if (now_near == _near) {
        _persist = 0;
    } else if (++_persist >= LIGHT_PROX_PERSIST) {
        _persist = 0;
        _near    = now_near;

        if (_near) {
            _near_since = now;
            _near_woke  = _asleep;   // sortir de veille ne vaut pas déclenchement
            _wake();
        } else {
            uint32_t held = now - _near_since;
            if (!_near_woke &&
                held >= LIGHT_GESTURE_MIN_MS && held <= LIGHT_GESTURE_MAX_MS &&
                now - _last_gesture >= LIGHT_GESTURE_COOLDOWN_MS) {
                _last_gesture = now;
                _gesture_trigger();
            }
        }
    }
    if (prox >= _thr_far) _last_present = now;

    // --- Veille : ni présence devant le capteur, ni entrée LVGL ---
    if (!_asleep &&
        now - _last_present > LIGHT_SLEEP_TIMEOUT_MS &&
        lv_display_get_inactive_time(NULL) > LIGHT_SLEEP_TIMEOUT_MS) {
        _asleep = true;
        display_backlight_sleep(true);
        log_line("[Light] Veille");
    }

    // --- Lumière ambiante, un poll sur LIGHT_ALS_EVERY_N ---
    if (_als_countdown) { _als_countdown--; return; }
    _als_countdown = LIGHT_ALS_EVERY_N - 1;

    uint8_t ab[4];
    if (!_apds_read(APDS_CH0DATA, ab, 4)) return;
    _last_ch0 = (uint16_t)ab[1] << 8 | ab[0];
    _last_ch1 = (uint16_t)ab[3] << 8 | ab[2];
    _last_clux = _clux_from(_last_ch0, _last_ch1);

    // Moyenne glissante : une ombre passagère ferait sinon clignoter l'écran.
    _clux_avg = (_clux_avg < 0.0f) ? (float)_last_clux
                                   : _clux_avg + ((float)_last_clux - _clux_avg) / 4.0f;

    if (!_auto_on || _asleep) return;
    if (_manual_hold) {
        if (now - _manual_t0 < LIGHT_MANUAL_HOLD_MS) return;
        _manual_hold = false;
        _force_apply = true;   // le manuel a bougé le duty dans notre dos
    }

    int pct   = _pct_from_clux((uint32_t)_clux_avg);
    int delta = pct > _applied_pct ? pct - _applied_pct : _applied_pct - pct;
    if (!_force_apply && delta < LIGHT_BRIGHTNESS_STEP) return;
    _force_apply = false;
    _applied_pct = pct;
    display_set_brightness_silent(pct);
}

// Réveille dans les DEUX sens : basculer le mode est une action sur le
// rétroéclairage, son effet doit être visible immédiatement.
void light_set_auto(bool on) {
    _auto_on     = on;
    _manual_hold = false;
    _force_apply = true;
    _wake();
    log_line("[Light] Luminosité %s", on ? "automatique" : "manuelle");
}

bool light_is_auto() { return _auto_on; }

void light_notify_manual() {
    _manual_hold = true;
    _manual_t0   = millis();
    _wake();
}

void light_wake() { _wake(); }

// ⚠️ La décision se prend ICI, pas dans _wake_on_interaction() : le compteur
// d'inactivité de LVGL n'est mis à jour qu'APRÈS que l'appui lui a été livré,
// donc trop tard pour l'intercepter.
bool light_touch_wake() {
    if (!_asleep) return false;
    _wake();
    return true;
}

void light_get_status(LightStatus* out) {
    uint32_t now = millis();
    out->present       = _present;
    out->prox          = _last_prox;
    out->prox_base     = _prox_base < 0.0f ? 0 : (uint16_t)_prox_base;
    out->thr_near      = _thr_near;
    out->thr_far       = _thr_far;
    out->clux          = _last_clux;
    out->ch0           = _last_ch0;
    out->ch1           = _last_ch1;
    out->near          = _near;
    out->auto_on       = _auto_on;
    out->manual_hold   = _manual_hold;
    out->asleep        = _asleep;
    out->brightness    = _applied_pct;
    out->gestures      = _gestures;
    out->since_gesture = _last_gesture ? now - _last_gesture : 0;
    out->since_present = now - _last_present;
    out->plugs         = _plugs;
}

void light_log_state() {
    if (!_present) { log_line("[Light] Capteur absent"); return; }
    log_line("[Light] %lu.%02lu lux, prox %u, %s%s%s",
             (unsigned long)(_last_clux / 100), (unsigned long)(_last_clux % 100),
             (unsigned)_last_prox,
             _auto_on ? "auto" : "manuel",
             _manual_hold ? " (suspendu)" : "",
             _asleep ? ", veille" : "");
}
