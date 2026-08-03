// ============================================================
// LOG_MANAGER.CPP — journal circulaire en mémoire RTC "no-init".
//
// Le buffer survit à un reset logiciel, un crash watchdog ou un ESP.restart() —
// seule une coupure d'alimentation le remet à zéro, donc log_init() ne le vide
// que sur un démarrage à froid.
//
// Appelé depuis plusieurs tâches (loop, audio, IA, HTTP, OTA) : accès protégé
// par un spinlock portMUX.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <esp_system.h>
#include <stdarg.h>

// ---- RESSOURCES LOCALES ----
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----

RTC_NOINIT_ATTR static char _log_buf[LOG_MAX_LINES][LOG_LINE_LEN];
RTC_NOINIT_ATTR static int  _log_head;
RTC_NOINIT_ATTR static int  _log_count;

static portMUX_TYPE _log_mux = portMUX_INITIALIZER_UNLOCKED;

// Pas en RTC_NOINIT : un redémarrage doit rendre le journal lisible par défaut.
static bool _loop_measure = false;

// ---- HELPERS ----

static const char* _reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "logiciel (ESP.restart)";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "watchdog interruption";
        case ESP_RST_TASK_WDT:  return "watchdog tâche";
        case ESP_RST_WDT:       return "watchdog (autre)";
        case ESP_RST_BROWNOUT:  return "brownout (alimentation)";
        default:                return "inconnue";
    }
}

// ---- API PUBLIQUES ----

void log_init() {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
        _log_head  = 0;
        _log_count = 0;
    }
    // À la toute première mise sous tension, la RTC contient n'importe quoi.
    if (_log_head < 0 || _log_head >= LOG_MAX_LINES)  _log_head  = 0;
    if (_log_count < 0 || _log_count > LOG_MAX_LINES) _log_count = 0;

    log_line("[BOOT] Raison du reset : %s", _reset_reason_str(reason));
}

void log_line(const char* fmt, ...) {
    char line[LOG_LINE_LEN];

    // Horodatage mm:ss.mmm en préfixe de CHAQUE ligne : sans lui, une suite de
    // lignes identiques ne distingue pas un phénomène continu d'une rafale.
    unsigned long ms = millis();
    int n = snprintf(line, sizeof(line), "%02lu:%02lu.%03lu ",
                     (ms / 60000UL) % 100, (ms / 1000UL) % 60, ms % 1000UL);

    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);

    // ⚠️ vsnprintf coupe à l'OCTET : sur un caractère UTF-8 multi-octets la
    // coupe tombe au milieu et /serial affiche du mojibake. On recule donc
    // jusqu'à une frontière — les octets de continuation valent 10xxxxxx.
    if (m > 0 && (size_t)m >= sizeof(line) - n) {
        int cut = LOG_LINE_LEN - 4;   // place pour "..." + '\0'
        while (cut > n && ((unsigned char)line[cut] & 0xC0) == 0x80) cut--;
        strcpy(line + cut, "...");
    }

    Serial.println(line);

    portENTER_CRITICAL(&_log_mux);
    strncpy(_log_buf[_log_head], line, LOG_LINE_LEN - 1);
    _log_buf[_log_head][LOG_LINE_LEN - 1] = '\0';
    _log_head = (_log_head + 1) % LOG_MAX_LINES;
    if (_log_count < LOG_MAX_LINES) _log_count++;
    portEXIT_CRITICAL(&_log_mux);
}

void log_clear() {
    portENTER_CRITICAL(&_log_mux);
    _log_head  = 0;
    _log_count = 0;
    portEXIT_CRITICAL(&_log_mux);
}

// JSON construit à la main, ligne par ligne : ArduinoJson demanderait le
// journal entier sur la pile de http_task. Contrepartie : une ligne peut être
// réécrite pendant le parcours, sans importance pour du diagnostic.
String log_get_json(int max_lines) {
    portENTER_CRITICAL(&_log_mux);
    int n     = (max_lines < _log_count) ? max_lines : _log_count;
    int start = (_log_head - n + LOG_MAX_LINES) % LOG_MAX_LINES;
    portEXIT_CRITICAL(&_log_mux);

    if (n <= 0) return String("[]");

    String out;
    out.reserve((size_t)n * (LOG_LINE_LEN + 8) + 2);   // + guillemets/virgules
    out += '[';

    char line[LOG_LINE_LEN];
    for (int i = 0; i < n; i++) {
        portENTER_CRITICAL(&_log_mux);
        memcpy(line, _log_buf[(start + i) % LOG_MAX_LINES], LOG_LINE_LEN);
        portEXIT_CRITICAL(&_log_mux);
        line[LOG_LINE_LEN - 1] = '\0';

        if (i) out += ',';
        out += '"';
        // Échappement JSON minimal : les logs contiennent des guillemets.
        for (const char* p = line; *p; p++) {
            if      (*p == '"')  out += "\\\"";
            else if (*p == '\\') out += "\\\\";
            else if ((unsigned char)*p < 0x20) out += ' ';   // pas de contrôle brut en JSON
            else out += *p;
        }
        out += '"';
    }
    out += ']';
    return out;
}

void log_set_loop_measure(bool on) {
    if (_loop_measure == on) return;
    _loop_measure = on;
    log_line("[LOOP] Mesure %s", on ? "activée" : "coupée");
    // Légende une fois à l'activation.
    if (on) log_line("[LOOP] X%%=travail/10s  Nt=tours de loop()  nom:ms=cumul par manager");
}

bool log_loop_measure() {
    return _loop_measure;
}
