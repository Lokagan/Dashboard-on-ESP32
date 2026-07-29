// ============================================================
// SYSINFO_MANAGER.CPP — écran de diagnostic système.
// Dessiné dans un TFT_eSprite hors-écran puis copié (memcpy) dans un
// lv_canvas affiché comme un écran LVGL normal. 6 pages, navigation
// tactile gauche/droite/centre.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <lvgl.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

// ---- RESSOURCES LOCALES ----
#include "sysinfo_manager.h"
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "wakeword_manager.h"
#include "littlefs_manager.h"
#include "log_manager.h"
// Pour les tailles de _si_alloc — chaque poste vient du header de son propriétaire
#include "display_manager.h"
#include "audio_manager.h"
#include "ai_companion.h"

// ---- OBJETS GLOBAUX ----

// Palette néon terminal
#define C_BG      0x0841
#define C_GRID    0x1082
#define C_CYAN    0x07FF
#define C_GREEN   0x07E0
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_RED     0xF800
#define C_WHITE   0xFFFF
#define C_DIM     0x4208
#define C_MAGENTA 0xF81F
#define C_MAGENTA_DK 0x780F   // magenta assombri — espace réellement occupé (spiffs/fat)
#define C_DKCYAN  0x0410

// Localisation mémoire (page MEMOIRE) : OÙ vit une allocation, un seul axe.
// Pas de rampe de criticité — choix assumé : orange ne doit pas signifier à la
// fois "interne" et "alarme". Jauges ET postes partagent ces deux teintes.
#define C_MEM_INT  C_ORANGE   // RAM interne
#define C_MEM_EXT  0x64BD     // PSRAM (externe) — bleu bleuet

#define SI_W   320
#define SI_H   240
#define SI_LH  12
#define SI_LX  8

#define SI_MAX_TASKS 32

#define SI_REFRESH_TICKS  20    // timer à 50 ms -> rafraîchissement 1 Hz
#define SI_TASK_LIVE_X   110    // début des colonnes vivantes (ETAT/PRI/%CPU/PILE)
#define SI_CPU_BAR_W      80    // barres de charge (page IDENTITE) — la moitié
                                // de la place dispo suffit et allège la ligne
#define SI_MEM_EVERY_N     2    // MEMOIRE : un rafraîchissement sur deux
#define SI_MEM_BAR_W      55    // barres MEM : largeur fixe (bornée par la valeur PSRAM, la plus longue)

extern TFT_eSPI _tft;   // instance partagée (display_manager.cpp) — sert
                        // uniquement à construire le sprite ci-dessous.

static TFT_eSprite _si_sprite(&_tft);          // surface de dessin hors-écran
static lv_obj_t*   _si_screen        = nullptr; // écran LVGL dédié (créé une fois)
static lv_obj_t*   _si_canvas        = nullptr; // canvas plein écran
static lv_color_t* _si_canvas_buf    = nullptr; // buffer PSRAM du canvas
static lv_obj_t*   _si_return_screen = nullptr; // écran à restaurer à la sortie
static lv_timer_t* _si_uptime_timer  = nullptr; // rafraîchit l'horodatage

enum SiPage {
    SI_PAGE_CHIP = 0,
    SI_PAGE_MEM,
    SI_PAGE_NET,
    SI_PAGE_TASKS,
    SI_PAGE_PART,
    SI_PAGE_FS,
    SI_PAGE_COUNT
};

static_assert(SI_PAGE_COUNT == SYSINFO_PAGE_COUNT,
              "SYSINFO_PAGE_COUNT (sysinfo_manager.h) doit suivre l'enum SiPage");

static int _si_page = SI_PAGE_CHIP;

// Snapshot précédent des compteurs d'exécution FreeRTOS, pour un %CPU en DELTA.
// ⚠️ Le compteur est un u32 de MICROSECONDES (RUN_TIME_STATS_USING_ESP_TIMER) :
// il déborde toutes les 71,6 min, un cumul depuis le boot serait donc faux au-
// delà. La soustraction non signée, elle, reste juste tant que l'écart entre
// deux relevés est plus court que ça — c'est toujours le cas ici (deux
// affichages de page).
static struct { TaskHandle_t h; uint32_t run; } _si_prev[SI_MAX_TASKS];
static UBaseType_t _si_prev_n  = 0;
static uint32_t    _si_prev_us = 0;

// Zones rafraîchies à 1 Hz par _si_uptime_timer_cb (blit PARTIEL).
// Ordre d'affichage des tâches FIGÉ à l'entrée sur la page : le tri est fait par
// %CPU, le rejouer à chaque seconde ferait sauter les lignes sous les yeux.
static TaskHandle_t _si_task_order[SI_MAX_TASKS];
static UBaseType_t  _si_task_shown  = 0;
static int          _si_task_y0     = 0;   // y de la 1re ligne de tâche
static int          _si_chip_load_y = 0;   // y de la ligne "Charge" (page IDENTITE)
static int          _si_mem_y0 = 0, _si_mem_y1 = 0;   // bornes du bloc vivant (page MEMOIRE)

// Miroir des tailles de config.h (PILES FREERTOS) — TaskStatus_t ne donne que le
// high-water, jamais la taille allouée : sans ce lookup, pas de pourcentage.
static const struct { const char* name; uint16_t stack; } _si_stacks[] = {
    { "loopTask",   STACK_BYTES_LOOP_TASK  },
    { "audio_task", STACK_BYTES_AUDIO_TASK },
    { "ai_task",    STACK_BYTES_AI_TASK    },
    { "http_task",  STACK_BYTES_HTTP_TASK  },
    { "mqtt_task",  STACK_BYTES_MQTT_TASK  },
    { "ota_task",   STACK_BYTES_OTA_TASK   },
};

// ---- HELPERS ----

// Helpers dessin de base
static void _si_fill_bg() {
    _si_sprite.fillScreen(C_BG);
    for (int x = 0; x < SI_W; x += 16)
        for (int y = 0; y < SI_H; y += 16)
            _si_sprite.drawPixel(x, y, C_GRID);
}

static void _si_hline(int y, uint16_t c = C_DIM) {
    _si_sprite.drawFastHLine(SI_LX, y, SI_W - SI_LX * 2, c);
}

static void _si_header(int page) {
    _si_sprite.fillRect(0, 0, SI_W, 26, C_BG);
    _si_sprite.drawFastHLine(0, 0, SI_W, C_CYAN);
    _si_sprite.drawFastHLine(0, 1, SI_W, C_DKCYAN);

    char pgbuf[32];
    snprintf(pgbuf, sizeof(pgbuf), "PG %d/%d", page + 1, SI_PAGE_COUNT);
    _si_sprite.setTextColor(C_YELLOW, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, 5);
    _si_sprite.print(pgbuf);

    _si_sprite.setTextColor(C_CYAN, C_BG);
    const char* t1 = "[ ES3C28P SYSTEM DIAGNOSTICS ]";
    _si_sprite.setCursor((SI_W - strlen(t1) * 6) / 2, 5);
    _si_sprite.print(t1);

    _si_sprite.setTextColor(C_DIM, C_BG);
    const char* t2 = "TFT_eSPI @ 40MHz  |  ILI9341V  |  ESP32-S3";
    _si_sprite.setCursor((SI_W - strlen(t2) * 6) / 2, 15);
    _si_sprite.print(t2);

    _si_sprite.drawFastHLine(0, 24, SI_W, C_DKCYAN);
    _si_sprite.drawFastHLine(0, 25, SI_W, C_CYAN);
}

static void _si_footer() {
    _si_sprite.drawFastHLine(0, SI_H - 13, SI_W, C_DKCYAN);
    _si_sprite.drawFastHLine(0, SI_H - 12, SI_W, C_CYAN);
    _si_sprite.setTextColor(C_DIM, C_BG);
    _si_sprite.setTextSize(1);

    _si_sprite.setCursor(SI_LX, SI_H - 9);
    _si_sprite.print("< PREV");

    const char* mid = "TAP CENTER = EXIT";
    _si_sprite.setCursor((SI_W - (int)strlen(mid) * 6) / 2, SI_H - 9);
    _si_sprite.print(mid);

    const char* right = "NEXT >";
    _si_sprite.setCursor(SI_W - SI_LX - (int)strlen(right) * 6, SI_H - 9);
    _si_sprite.print(right);
}

static void _si_clear_content() {
    _si_sprite.fillRect(0, 27, SI_W, SI_H - 27 - 14, C_BG);
}

static void _si_uptime() {
    unsigned long ms = millis();
    unsigned long s  = ms / 1000;
    unsigned long m  = s / 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu.%03lu", m, s % 60, ms % 1000);
    _si_sprite.setTextColor(C_YELLOW, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_W - 64, 5);
    _si_sprite.print(buf);
}

static void _si_progress_bar(int x, int y, int w, int h, float pct, uint16_t c) {
    _si_sprite.drawRect(x, y, w, h, C_DIM);
    int fill = (int)((w - 2) * constrain(pct, 0.0f, 1.0f));
    _si_sprite.fillRect(x + 1, y + 1, fill,             h - 2, c);
    _si_sprite.fillRect(x + 1 + fill, y + 1, w - 2 - fill, h - 2, C_BG);
}

// Ligne "clé : valeur", avec barre optionnelle. L'espace de 6 px après la clé
// est la largeur de l'ancien curseur clignotant, gardée pour ne pas décaler la
// mise en page.
// barMaxW : largeur max de la barre en px, 0 = jusqu'à la marge droite.
static void _si_row(int y, const char* key, const char* val,
                    uint16_t valColor = C_GREEN, bool withBar = false,
                    float barPct = 0, uint16_t barColor = C_CYAN,
                    int barMaxW = 0, bool barRight = false) {
    _si_sprite.setTextColor(C_DKCYAN, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, y);
    _si_sprite.print(key);

    _si_sprite.setCursor(_si_sprite.getCursorX() + 6, y);
    _si_sprite.setTextColor(valColor, C_BG);
    _si_sprite.print(val);

    if (withBar) {
        int bx, bw;
        if (barRight && barMaxW > 0) {   // largeur FIXE, alignée sur la marge droite
            bw = barMaxW;                 // (barres MEM : mêmes x/largeur malgré des valeurs de longueur variable)
            bx = SI_W - SI_LX - barMaxW;
        } else {                          // après le texte, éventuellement plafonnée (barres CPU IDENTITE)
            bx = _si_sprite.getCursorX() + 4;
            bw = SI_W - bx - SI_LX;
            if (barMaxW > 0 && bw > barMaxW) bw = barMaxW;
        }
        if (bw > 10) _si_progress_bar(bx, y + 1, bw, 7, barPct, barColor);
    }
}

static void _si_section(int y, const char* title) {
    _si_sprite.setTextColor(C_CYAN, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, y);
    _si_sprite.print(title);
}

// Sprite -> canvas. Même format des deux côtés (RGB565, ordre natif TFT_eSPI) :
// simple memcpy, aucune conversion.
static void _si_blit() {
    if (!_si_canvas_buf) return;
    memcpy(_si_canvas_buf, _si_sprite.getPointer(), (size_t)SI_W * SI_H * 2);
    lv_obj_invalidate(_si_canvas);
}

// Ne recopie qu'un rectangle plutôt que les 150 Ko du canvas entier — c'est ce
// qui rend les rafraîchissements 1 Hz abordables.
static void _si_blit_rect(int x, int y, int w, int h) {
    if (!_si_canvas_buf) return;
    const uint8_t* srcBase = (const uint8_t*)_si_sprite.getPointer();
    uint8_t*       dstBase = (uint8_t*)_si_canvas_buf;
    for (int row = 0; row < h; row++) {
        size_t offset = (size_t)((y + row) * SI_W + x) * 2;
        memcpy(dstBase + offset, srcBase + offset, (size_t)w * 2);
    }
    lv_area_t area = { x, y, x + w - 1, y + h - 1 };
    lv_obj_invalidate_area(_si_canvas, &area);
}

// Helpers — formatage
static const char* _si_chip_model_str(esp_chip_model_t m) {
    switch (m) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32C2: return "ESP32-C2";
        case CHIP_ESP32C6: return "ESP32-C6";
        case CHIP_ESP32H2: return "ESP32-H2";
        default:           return "inconnu";
    }
}

static const char* _si_flash_mode_str(FlashMode_t m) {
    switch (m) {
        case FM_QIO:  return "QIO";
        case FM_QOUT: return "QOUT";
        case FM_DIO:  return "DIO";
        case FM_DOUT: return "DOUT";
        default:      return "?";
    }
}

static const char* _si_reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWER-ON";
        case ESP_RST_EXT:       return "PIN EXTERNE";
        case ESP_RST_SW:        return "LOGICIEL (reboot)";
        case ESP_RST_PANIC:     return "PANIC / EXCEPTION";
        case ESP_RST_INT_WDT:   return "WDT INTERRUPTION";
        case ESP_RST_TASK_WDT:  return "WDT TACHE";
        case ESP_RST_WDT:       return "WDT (autre)";
        case ESP_RST_DEEPSLEEP: return "REVEIL DEEP SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (alim)";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "INCONNU";
    }
}

static const char* _si_part_subtype_str(uint8_t type, uint8_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
        if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            subtype <  ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            static char buf[8];
            snprintf(buf, sizeof(buf), "ota_%d", subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
            return buf;
        }
        return "app ?";
    }
    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "otadata";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:   return "spiffs";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:      return "fat";
            default:                                  return "data ?";
        }
    }
    return "?";
}

// Taille de pile allouée, 0 si inconnue (tâche système)
static uint16_t _si_stack_size(const char* name) {
    for (auto const& s : _si_stacks)
        if (strcmp(s.name, name) == 0) return s.stack;
    return 0;
}

// Échantillonne les compteurs d'exécution et met à jour le snapshot.
//   pct  (optionnel) : rempli avec le %CPU de chaque tâche de st[]
//   core0/core1      : charge de chaque cœur, déduite du temps des tâches IDLE
// Retourne false si la fenêtre de mesure n'est pas exploitable (premier relevé
// au-delà du débordement du compteur, ou fenêtre trop courte).
static bool _si_cpu_sample(TaskStatus_t* st, UBaseType_t n, uint8_t* pct,
                           int* core0, int* core1) {
    uint32_t now_us = (uint32_t)esp_timer_get_time();   // même troncature u32 que le compteur
    bool     first  = (_si_prev_n == 0);
    uint32_t dt_us  = first ? now_us : (now_us - _si_prev_us);

    // Premier relevé : on ne peut rapporter qu'au temps depuis le boot, ce qui
    // n'a plus de sens une fois le compteur rebouclé.
    bool valid = (dt_us > 1000) && !(first && millis() > 71UL * 60 * 1000);

    uint32_t idle_us[2] = { 0, 0 };

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t prev = 0;
        for (UBaseType_t j = 0; j < _si_prev_n; j++) {
            if (_si_prev[j].h == st[i].xHandle) { prev = _si_prev[j].run; break; }
        }
        uint32_t d = st[i].ulRunTimeCounter - prev;   // non signé : robuste au wrap
        uint32_t p = valid ? (uint32_t)(((uint64_t)d * 100) / dt_us) : 0;
        if (p > 100) p = 100;
        if (pct) pct[i] = (uint8_t)p;

        int c = (int)st[i].xCoreID;
        if (strncmp(st[i].pcTaskName, "IDLE", 4) == 0 && (c == 0 || c == 1)) idle_us[c] = d;
    }

    if (core0) *core0 = valid ? 100 - (int)((uint64_t)idle_us[0] * 100 / dt_us) : -1;
    if (core1) *core1 = valid ? 100 - (int)((uint64_t)idle_us[1] * 100 / dt_us) : -1;
    if (core0 && *core0 < 0 && valid) *core0 = 0;
    if (core1 && *core1 < 0 && valid) *core1 = 0;

    // Nouveau snapshot
    _si_prev_n = (n < SI_MAX_TASKS) ? n : SI_MAX_TASKS;
    for (UBaseType_t i = 0; i < _si_prev_n; i++) {
        _si_prev[i].h   = st[i].xHandle;
        _si_prev[i].run = st[i].ulRunTimeCounter;
    }
    _si_prev_us = now_us;

    return valid;
}

// ---- API LOCALES ----

// --- PAGE 1 — IDENTITE ---

// Lignes "Charge" — un cœur par ligne, avec barre. Efface avant de réécrire :
// appelée aussi en rafraîchissement, par-dessus les anciennes valeurs.
static void _si_draw_cpu_load(int y, int c0, int c1) {
    char buf[16];
    const int   pct[2]  = { c0, c1 };
    const char* keys[2] = { "  Charge Core 0 : ", "  Charge Core 1 : " };

    _si_sprite.fillRect(0, y, SI_W, SI_LH * 2, C_BG);
    for (int i = 0; i < 2; i++) {
        if (pct[i] >= 0) snprintf(buf, sizeof(buf), "%3d%%", pct[i]);
        else             snprintf(buf, sizeof(buf), "  --");
        uint16_t c = (pct[i] < 0) ? C_DIM : (pct[i] > 90) ? C_ORANGE : C_GREEN;
        _si_row(y + i * SI_LH, keys[i], buf, c,
                pct[i] >= 0, pct[i] / 100.0f, c, SI_CPU_BAR_W);
    }
}

static void _si_page_chip() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[48];
    int y = 30;

    _si_section(y, ">> IDENTITE"); y += SI_LH;

    _si_row(y, "  Modele        : ", _si_chip_model_str(chip.model), C_WHITE); y += SI_LH;

    snprintf(buf, sizeof(buf), "rev %d", chip.revision);
    _si_row(y, "  Silicium      : ", buf, C_WHITE); y += SI_LH;

    snprintf(buf, sizeof(buf), "%d coeurs @ %d MHz", chip.cores, getCpuFrequencyMhz());
    _si_row(y, "  CPU           : ", buf, C_WHITE); y += SI_LH;

    // Charge instantanée, mesurée entre cet affichage et le précédent
    // (cf. _si_cpu_sample). "--" tant qu'aucune fenêtre exploitable.
    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);
    int c0 = -1, c1 = -1;
    if (n > 0) _si_cpu_sample(st, n, nullptr, &c0, &c1);

    _si_chip_load_y = y;
    _si_draw_cpu_load(y, c0, c1); y += SI_LH * 2;

    snprintf(buf, sizeof(buf), "%s", ESP.getSdkVersion());
    _si_row(y, "  IDF/Arduino   : ", buf, C_YELLOW); y += SI_LH;

    _si_hline(y); y += 3;
    _si_section(y, ">> FLASH"); y += SI_LH;

    snprintf(buf, sizeof(buf), "%d MB", ESP.getFlashChipSize() / (1024 * 1024));
    _si_row(y, "  Taille        : ", buf, C_YELLOW); y += SI_LH;

    snprintf(buf, sizeof(buf), "%lu MHz - %s",
             (unsigned long)(ESP.getFlashChipSpeed() / 1000000),
             _si_flash_mode_str(ESP.getFlashChipMode()));
    _si_row(y, "  Frequence     : ", buf, C_WHITE); y += SI_LH;

    _si_hline(y); y += 3;
    _si_section(y, ">> ETAT"); y += SI_LH;

    float tempCpu = temperatureRead();
    int   tempInt = (int)tempCpu;
    uint16_t tempColor = tempInt < 50 ? C_GREEN : tempInt < 70 ? C_YELLOW : C_RED;
    snprintf(buf, sizeof(buf), "%d C (die)", tempInt);
    _si_row(y, "  Temp CPU      : ", buf, tempColor); y += SI_LH;

    esp_reset_reason_t rr = esp_reset_reason();
    bool bad = (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
                rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
    uint16_t rrColor = bad ? C_RED : (rr == ESP_RST_POWERON || rr == ESP_RST_SW) ? C_GREEN : C_YELLOW;
    _si_row(y, "  Dernier rst   : ", _si_reset_reason_str(rr), rrColor); y += SI_LH;
}

// --- PAGE 2 — MEMOIRE ---

// Postes d'allocation connus du firmware — buffers/objets UNIQUEMENT, PAS les
// piles de tâches (celles-ci vivent dans _si_stacks, comptées à part dans le
// bilan mémoire pour ne pas doublonner). La LISTE reste tenue à la main —
// l'ESP-IDF ne sait pas dire qui a alloué quoi sans CONFIG_HEAP_TASK_TRACKING,
// absent des libs Arduino précompilées — mais chaque TAILLE vient désormais du
// header de son propriétaire : elles ne peuvent plus diverger sans que le
// compilateur le voie. Les littéraux d'origine avaient déjà dérivé deux fois
// (LVGL sous-déclaré de 3 840 o, payload MQTT compté en interne alors qu'il est
// en PSRAM).
static const struct { const char* label; bool internal; uint32_t bytes; } _si_alloc[] = {
    { "LVGL",     true,  LV_BUF_BYTES },                       // buffer de dessin
    { "MQTT out", true,  MQTT_OUT_BUFFER_SIZE },               // buffer d'émission (interne)
    { "MQTT buf", false, MQTT_BUFFER_SIZE },                   // buffer RX (PSRAM via seuil ALWAYSINTERNAL)
    { "Reassemb.",false, MQTT_BUFFER_SIZE },                   // staging PSRAM du payload
    { "Capture",  false, AUDIO_RECORD_CAPACITY_SAMPLES * 2 },  // buffer d'enregistrement
    { "Avatar",   false, COMPANION_PSRAM_BYTES },              // frames Companion
    { "JSON",     false, JSON_TOTAL_SIZE },                    // tables
    // Sprite/Canvas EN DERNIER : alloués seulement à la 1re ouverture de SysInfo
    // (paresseux) — le bilan mémoire les affiche à 0 tant que _si_screen == null.
    { "Sprite",   false, (uint32_t)SI_W * SI_H * 2 },          // sprite SysInfo
    { "Canvas",   false, (uint32_t)SI_W * SI_H * 2 },          // canvas SysInfo
};

// Partie VIVANTE de la page (les postes connus en dessous sont une table
// statique). Retourne le y atteint.
static int _si_draw_mem_live(int y) {
    char buf[48];

    _si_section(y, ">> HEAP INTERNE"); y += SI_LH;

    size_t intFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t intMin   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    float  intPct   = intTotal ? 1.0f - (float)intFree / intTotal : 0;

    snprintf(buf, sizeof(buf), "%lu KB / %lu KB libre",
             (unsigned long)intFree / 1024, (unsigned long)intTotal / 1024);
    _si_row(y, "  Libre        : ", buf, C_GREEN, true, intPct, C_MEM_INT, SI_MEM_BAR_W, true); y += SI_LH;

    snprintf(buf, sizeof(buf), "%lu KB (pire cas atteint)", (unsigned long)intMin / 1024);
    _si_row(y, "  Min. atteint : ", buf, C_YELLOW); y += SI_LH;

    size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    snprintf(buf, sizeof(buf), "%lu KB libre", (unsigned long)dmaFree / 1024);
    _si_row(y, "  Capable DMA  : ", buf, C_MEM_INT); y += SI_LH;   // sous-ensemble de l'interne

    _si_hline(y); y += 3;
    _si_section(y, ">> PSRAM"); y += SI_LH;

    if (psramFound()) {
        size_t psFree  = ESP.getFreePsram();
        size_t psTotal = ESP.getPsramSize();
        size_t psMin   = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        float  psPct   = psTotal ? 1.0f - (float)psFree / psTotal : 0;

        snprintf(buf, sizeof(buf), "%lu KB / %lu KB libre",
                 (unsigned long)psFree / 1024, (unsigned long)psTotal / 1024);
        _si_row(y, "  Libre        : ", buf, C_GREEN, true, psPct, C_MEM_EXT, SI_MEM_BAR_W, true); y += SI_LH;

        snprintf(buf, sizeof(buf), "%lu KB (pire cas atteint)", (unsigned long)psMin / 1024);
        _si_row(y, "  Min. atteint : ", buf, C_YELLOW); y += SI_LH;
    } else {
        _si_row(y, "  Etat         : ", "NON DETECTEE", C_RED); y += SI_LH;
    }

    size_t sketchSize  = ESP.getSketchSize();
    size_t sketchTotal = ESP.getFreeSketchSpace() + sketchSize;
    float  sketchPct   = sketchTotal ? (float)sketchSize / sketchTotal : 0;
    snprintf(buf, sizeof(buf), "%lu KB / %lu KB",
             (unsigned long)sketchSize / 1024, (unsigned long)sketchTotal / 1024);
    _si_row(y, "  Firmware     : ", buf, C_YELLOW, true, sketchPct, C_MAGENTA, SI_MEM_BAR_W, true); y += SI_LH;

    return y;
}

static void _si_page_mem() {
    char buf[48];

    _si_mem_y0 = 30;
    int y = _si_draw_mem_live(_si_mem_y0);
    _si_mem_y1 = y;

    _si_hline(y); y += 3;

    // Titre + légende de couleur : les deux mots dans leur teinte SERVENT de clé
    // aux postes en dessous (orange = interne, bleu = PSRAM). print() enchaîne
    // depuis le curseur laissé par le précédent.
    _si_sprite.setTextSize(1);
    _si_sprite.setTextColor(C_CYAN, C_BG);
    _si_sprite.setCursor(SI_LX, y);
    _si_sprite.print(">> POSTES CONNUS   ");
    _si_sprite.setTextColor(C_MEM_INT, C_BG);
    _si_sprite.print("interne  ");
    _si_sprite.setTextColor(C_MEM_EXT, C_BG);
    _si_sprite.print("PSRAM");
    y += SI_LH;

    // Deux colonnes compactes : "LVGL 7K int" / "Avatar 886K"
    // COL1 s'aligne sur _si_row, dont les clés commencent par deux espaces —
    // sans ce décalage le bloc paraissait rentré à gauche du reste de la page.
    const int COL1 = SI_LX + 2 * 6;
    const int COL2 = COL1 + 152;
    int nb = sizeof(_si_alloc) / sizeof(_si_alloc[0]);
    for (int i = 0; i < nb; i++) {
        int colX = (i % 2 == 0) ? COL1 : COL2;
        int rowY = y + (i / 2) * SI_LH;

        // Interne/externe est porté par la COULEUR (cf. légende) : plus de
        // suffixe "int", il faisait doublon.
        snprintf(buf, sizeof(buf), "%-8s %4luK", _si_alloc[i].label,
                 (unsigned long)(_si_alloc[i].bytes / 1024));
        _si_sprite.setTextColor(_si_alloc[i].internal ? C_MEM_INT : C_MEM_EXT, C_BG);
        _si_sprite.setTextSize(1);
        _si_sprite.setCursor(colX, rowY);
        _si_sprite.print(buf);
    }
}

// --- PAGE 3 — RESEAU ---

static void _si_page_net() {
    char buf[48];
    int y = 30;

    _si_section(y, ">> WIFI"); y += SI_LH;

    if (wifi_is_connected()) {
        _si_row(y, "  SSID        : ", WiFi.SSID().c_str(), C_GREEN); y += SI_LH;
        _si_row(y, "  IP          : ", WiFi.localIP().toString().c_str(), C_WHITE); y += SI_LH;
        _si_row(y, "  Passerelle  : ", WiFi.gatewayIP().toString().c_str(), C_WHITE); y += SI_LH;
        _si_row(y, "  Masque      : ", WiFi.subnetMask().toString().c_str(), C_WHITE); y += SI_LH;
        _si_row(y, "  DNS         : ", WiFi.dnsIP().toString().c_str(), C_WHITE); y += SI_LH;
        _si_row(y, "  MAC         : ", WiFi.macAddress().c_str(), C_DIM); y += SI_LH;

        snprintf(buf, sizeof(buf), "canal %d", WiFi.channel());
        _si_row(y, "  Wifi canal  : ", buf, C_WHITE); y += SI_LH;

        int rssi = WiFi.RSSI();
        uint16_t rssiColor = rssi > -60 ? C_GREEN : rssi > -75 ? C_YELLOW : C_RED;
        snprintf(buf, sizeof(buf), "%d dBm", rssi);
        _si_row(y, "  RSSI        : ", buf, rssiColor); y += SI_LH;
    } else {
        _si_row(y, "  Etat        : ", "DECONNECTE", C_RED); y += SI_LH;
    }

    _si_hline(y); y += 3;
    _si_section(y, ">> MQTT"); y += SI_LH;

    if (mqtt_is_connected()) {
        snprintf(buf, sizeof(buf), "%s:%d", MQTT_BROKER, MQTT_PORT);
        _si_row(y, "  Broker       : ", buf, C_GREEN); y += SI_LH;
        _si_row(y, "  Client ID    : ", MQTT_CLIENT_ID, C_WHITE); y += SI_LH;
    } else {
        _si_row(y, "  Etat         : ", "DECONNECTE", C_RED); y += SI_LH;
    }
}

// --- PAGE 4 — TACHES ---
// Une colonne riche : NOM, cœur, état, priorité, %CPU, pile.
// Etats : X=actif  R=pret  B=bloque  S=suspendu  D=supprime
//
// Les tâches IDLE sont EXCLUES de la liste : leur temps d'exécution est
// précisément ce qui sert à calculer la charge par cœur affichée en tête,
// l'afficher deux fois ne dirait rien de plus et coûterait deux lignes.
//
// Tri par ÉTAT puis %CPU décroissant. 
// La liste dépasse toujours la hauteur disponible : ce qui est coupé doit être
// ce qui n'apprend rien, donc les tâches bloquées et inactives, jamais celles qui consomment.

static char _si_task_state_char(eTaskState s) {
    switch (s) {
        case eRunning:   return 'X';
        case eReady:     return 'R';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

// Rang de tri : plus c'est bas, plus la tâche mérite d'être vue. Elle tourne >
// elle attend son tour > elle dort sur un sémaphore > elle est suspendue.
static uint8_t _si_task_state_rank(eTaskState s) {
    switch (s) {
        case eRunning:   return 0;
        case eReady:     return 1;
        case eBlocked:   return 2;
        case eSuspended: return 3;
        case eDeleted:   return 4;
        default:         return 5;
    }
}

// Colonnes VIVANTES d'une ligne (le nom et le cœur, eux, ne bougent pas).
// Efface avant d'écrire : appelée aussi en rafraîchissement.
static void _si_draw_task_live(int rowY, const TaskStatus_t* t, uint8_t pct, bool cpu_ok) {
    char buf[32];
    _si_sprite.fillRect(SI_TASK_LIVE_X, rowY, SI_W - SI_TASK_LIVE_X, SI_LH, C_BG);
    _si_sprite.setTextSize(1);

    if (!t) {   // tâche disparue depuis l'entrée sur la page
        _si_sprite.setTextColor(C_DIM, C_BG);
        _si_sprite.setCursor(SI_TASK_LIVE_X, rowY);
        _si_sprite.print("(terminee)");
        return;
    }

    _si_sprite.setTextColor(C_DKCYAN, C_BG);
    _si_sprite.setCursor(SI_TASK_LIVE_X, rowY);
    _si_sprite.print(_si_task_state_char(t->eCurrentState));

    snprintf(buf, sizeof(buf), "%2u", (unsigned)t->uxCurrentPriority);
    _si_sprite.setCursor(124, rowY);
    _si_sprite.print(buf);

    // %CPU — orange au-delà de 30 %, la tâche mérite alors qu'on la regarde
    if (cpu_ok) {
        snprintf(buf, sizeof(buf), "%3u%%", (unsigned)pct);
        _si_sprite.setTextColor(pct > 30 ? C_ORANGE : C_WHITE, C_BG);
    } else {
        snprintf(buf, sizeof(buf), "  --");
        _si_sprite.setTextColor(C_DIM, C_BG);
    }
    _si_sprite.setCursor(154, rowY);
    _si_sprite.print(buf);

    // Pile : high-water (octets LIBRES). Rouge < 512 o, orange < 1 Ko.
    // Le total n'est connu que pour NOS tâches (cf. _si_stacks).
    unsigned long freeBytes = (unsigned long)t->usStackHighWaterMark * sizeof(StackType_t);
    uint16_t stackTotal = _si_stack_size(t->pcTaskName);
    if (stackTotal) snprintf(buf, sizeof(buf), "%lu/%u", freeBytes, (unsigned)stackTotal);
    else            snprintf(buf, sizeof(buf), "%lu", freeBytes);

    _si_sprite.setTextColor((freeBytes < 512) ? C_RED : (freeBytes < 1024) ? C_ORANGE : C_GREEN, C_BG);
    _si_sprite.setCursor(198, rowY);
    _si_sprite.print(buf);
}

static void _si_page_tasks() {
    char buf[64];
    int y = 30;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);

    if (n == 0) {
        _si_sprite.setTextColor(C_RED, C_BG);
        _si_sprite.setCursor(SI_LX, y);
        _si_sprite.print("(uxTaskGetSystemState: buffer trop petit)");
        return;
    }

    uint8_t pct[SI_MAX_TASKS];
    int c0 = -1, c1 = -1;
    bool cpu_ok = _si_cpu_sample(st, n, pct, &c0, &c1);

    // En-tête sur UNE ligne : la place manque ici (chaque ligne prise à
    // l'en-tête est une tâche de moins affichée).
    // Le détail par cœur, avec barres, est sur la page IDENTITE
    size_t heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (cpu_ok) snprintf(buf, sizeof(buf), "Core0 %3d%%  Core1 %3d%%  |  %u tach.  |  heap %luK",
                         c0, c1, (unsigned)n, (unsigned long)heapFree / 1024);
    else        snprintf(buf, sizeof(buf), "Core --  (premiere mesure)  |  %u tach.  |  heap %luK",
                         (unsigned)n, (unsigned long)heapFree / 1024);
    _si_sprite.setTextColor(C_CYAN, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, y);
    _si_sprite.print(buf);
    y += SI_LH;

    _si_hline(y); y += 5;

    // Écarte les IDLE, en compactant st[]/pct[] au passage
    UBaseType_t m = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strncmp(st[i].pcTaskName, "IDLE", 4) == 0) continue;
        st[m]  = st[i];
        pct[m] = pct[i];
        m++;
    }

    // Tri : état croissant (running d'abord), puis %CPU décroissant. Ce qui
    // déborde de la page est donc ce qui dort et ne consomme rien.
    for (UBaseType_t i = 0; i < m; i++) {
        for (UBaseType_t j = i + 1; j < m; j++) {
            uint8_t ri = _si_task_state_rank(st[i].eCurrentState);
            uint8_t rj = _si_task_state_rank(st[j].eCurrentState);
            bool swap = (rj < ri) || (rj == ri && pct[j] > pct[i]);
            if (swap) {
                TaskStatus_t t = st[i]; st[i] = st[j]; st[j] = t;
                uint8_t p = pct[i]; pct[i] = pct[j]; pct[j] = p;
            }
        }
    }

    // Titres de colonnes
    _si_sprite.setTextColor(C_DKCYAN, C_BG);
    _si_sprite.setCursor(SI_LX, y);  _si_sprite.print("NOM");
    _si_sprite.setCursor(96,    y);  _si_sprite.print("C");
    _si_sprite.setCursor(110,   y);  _si_sprite.print("E");
    _si_sprite.setCursor(124,   y);  _si_sprite.print("PRI");
    _si_sprite.setCursor(154,   y);  _si_sprite.print("%CPU");
    _si_sprite.setCursor(198,   y);  _si_sprite.print("PILE LIBRE/TOTAL");
    y += SI_LH;

    UBaseType_t maxRows = (UBaseType_t)((SI_H - 14 - y) / SI_LH);
    // Si tout ne tient pas, la DERNIÈRE ligne sert au compteur de reste : il doit
    // rester dans la zone effacée par _si_clear_content (jusqu'à y=226), sinon il
    // se dessine sur le pied de page et n'en repart jamais.
    UBaseType_t shown = (m <= maxRows) ? m : (maxRows > 0 ? maxRows - 1 : 0);

    // Ordre figé pour les rafraîchissements 1 Hz (cf. _si_refresh_tasks)
    _si_task_y0    = y;
    _si_task_shown = shown;

    for (UBaseType_t i = 0; i < shown; i++) {
        int rowY = y + (int)(i * SI_LH);
        _si_task_order[i] = st[i].xHandle;

        _si_sprite.setTextColor(C_WHITE, C_BG);
        snprintf(buf, sizeof(buf), "%.14s", st[i].pcTaskName ? st[i].pcTaskName : "?");
        _si_sprite.setCursor(SI_LX, rowY);
        _si_sprite.print(buf);

        int core = (int)st[i].xCoreID;
        _si_sprite.setTextColor(C_DKCYAN, C_BG);
        _si_sprite.setCursor(96, rowY);
        _si_sprite.print((core == 0 || core == 1) ? (char)('0' + core) : '*');

        _si_draw_task_live(rowY, &st[i], pct[i], cpu_ok);
    }

    if (m > shown) {
        _si_sprite.setTextColor(C_DIM, C_BG);
        _si_sprite.setCursor(SI_LX, y + (int)(shown * SI_LH));
        snprintf(buf, sizeof(buf), "+ %u autre(s) non affichee(s)", (unsigned)(m - shown));
        _si_sprite.print(buf);
    }
}

// --- Rafraîchissements 1 Hz (blit PARTIEL, seule la zone réécrite est invalidée) ---
//
// ⚠️ Le %CPU est mesuré ENTRE DEUX RENDUS : rafraîchir à 1 Hz donne donc une
// charge instantanée (c'est le but), mais le coût du rendu lui-même compte dans
// loopTask, qui apparaît plus chargée qu'au repos. Même biais que htop.

static void _si_refresh_chip() {
    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);
    if (n == 0) return;

    int c0 = -1, c1 = -1;
    _si_cpu_sample(st, n, nullptr, &c0, &c1);

    _si_draw_cpu_load(_si_chip_load_y, c0, c1);
    _si_blit_rect(0, _si_chip_load_y, SI_W, SI_LH * 2);
}

// ⚠️ Blit PLEINE LARGEUR, volontairement. Restreindre aux seules colonnes de
// valeurs (x >= 110) a été essayé et MESURÉ : -33 % de pixels pour -3 % de
// temps seulement. Le coût du canvas est dominé par l'accès PSRAM, pas par le
// nombre de pixels — une bande pleine largeur se lit en 640 octets contigus par
// ligne, une tranche étroite lit 420 octets puis en saute 220, et l'efficacité
// des rafales s'effondre. Ne pas refaire : ici, découper en largeur COÛTE.
static void _si_refresh_mem() {
    if (_si_mem_y1 <= _si_mem_y0) return;
    int h = _si_mem_y1 - _si_mem_y0;
    _si_sprite.fillRect(0, _si_mem_y0, SI_W, h, C_BG);
    _si_draw_mem_live(_si_mem_y0);
    _si_blit_rect(0, _si_mem_y0, SI_W, h);
}

static void _si_refresh_tasks() {
    if (_si_task_shown == 0) return;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);
    if (n == 0) return;

    uint8_t pct[SI_MAX_TASKS];
    bool cpu_ok = _si_cpu_sample(st, n, pct, nullptr, nullptr);

    // Ordre figé : on retrouve chaque tâche par son handle.
    for (UBaseType_t i = 0; i < _si_task_shown; i++) {
        const TaskStatus_t* found = nullptr;
        uint8_t p = 0;
        for (UBaseType_t j = 0; j < n; j++) {
            if (st[j].xHandle == _si_task_order[i]) { found = &st[j]; p = pct[j]; break; }
        }
        _si_draw_task_live(_si_task_y0 + (int)(i * SI_LH), found, p, cpu_ok);
    }

    _si_blit_rect(SI_TASK_LIVE_X, _si_task_y0,
                  SI_W - SI_TASK_LIVE_X, (int)(_si_task_shown * SI_LH));
}

// --- PAGE 5 — PARTITIONS FLASH ---
// Table des partitions + partition de boot, puis un graphe en barre empilée
// représentant l'occupation de la flash entière.

static uint16_t _si_part_color(uint8_t type, uint8_t subtype, bool isRunning) {
    if (type == ESP_PARTITION_TYPE_APP) return isRunning ? C_GREEN : C_DKCYAN;
    switch (subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_NVS:      return C_YELLOW;
        case ESP_PARTITION_SUBTYPE_DATA_OTA:      return C_ORANGE;
        case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
        case ESP_PARTITION_SUBTYPE_DATA_FAT:      return C_MAGENTA;
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return C_RED;
        default:                                  return C_DIM;
    }
}

static void _si_page_partitions() {
    char buf[64];
    int y = 30;

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();

    _si_section(y, ">> BOOT"); y += SI_LH;

    if (running) {
        snprintf(buf, sizeof(buf), "%s (offset 0x%06X)", running->label, (unsigned)running->address);
        _si_row(y, "  Active (en cours) : ", buf, C_GREEN); y += SI_LH;
    } else {
        _si_row(y, "  Active (en cours) : ", "INCONNU", C_RED); y += SI_LH;
    }

    if (boot && boot != running) {
        snprintf(buf, sizeof(buf), "%s (prochain reboot)", boot->label);
        _si_row(y, "  Boot programme     : ", buf, C_YELLOW); y += SI_LH;
    }

    _si_hline(y); y += 5;

    _si_sprite.setTextColor(C_DKCYAN, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, y);   _si_sprite.print("LABEL");
    _si_sprite.setCursor(95, y);      _si_sprite.print("TYPE");
    _si_sprite.setCursor(150, y);     _si_sprite.print("TAILLE");
    _si_sprite.setCursor(210, y);     _si_sprite.print("OFFSET");
    y += SI_LH;

    // Réserve la place du graphe avant de compter les lignes de tableau
    const int graphH     = 16;
    const int legendH    = 18;
    const int graphBlock = 8 + graphH + 4 + legendH;
    int maxRows = (SI_H - 14 - graphBlock - y) / SI_LH;
    if (maxRows < 1) maxRows = 1;

    int shown = 0;

    struct PartGraphEntry {
        uint32_t address;
        uint32_t size;
        uint16_t color;
        bool     isRunning;
        float    usedFrac;   // 0..1 = occupé (spiffs/fat) ; -1 = non applicable
    };
    static const int MAX_GRAPH_PARTS = 16;
    PartGraphEntry graphEntries[MAX_GRAPH_PARTS];
    int graphCount = 0;
    uint32_t flashTotal = ESP.getFlashChipSize();

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr) {
        const esp_partition_t* p = esp_partition_get(it);
        bool isRunning = running && (p->address == running->address);

        if (shown < maxRows) {
            _si_sprite.setTextColor(isRunning ? C_GREEN : C_WHITE, C_BG);
            _si_sprite.setCursor(SI_LX, y);
            _si_sprite.print(p->label);
            if (isRunning) _si_sprite.print("*");

            _si_sprite.setTextColor(C_CYAN, C_BG);
            _si_sprite.setCursor(95, y);
            _si_sprite.print(_si_part_subtype_str(p->type, p->subtype));

            _si_sprite.setTextColor(C_YELLOW, C_BG);
            _si_sprite.setCursor(150, y);
            snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(p->size / 1024));
            _si_sprite.print(buf);

            _si_sprite.setTextColor(C_DIM, C_BG);
            _si_sprite.setCursor(210, y);
            snprintf(buf, sizeof(buf), "0x%06X", (unsigned)p->address);
            _si_sprite.print(buf);

            y += SI_LH;
            shown++;
        }

        if (graphCount < MAX_GRAPH_PARTS) {
            graphEntries[graphCount].address   = p->address;
            graphEntries[graphCount].size      = p->size;
            graphEntries[graphCount].color     = _si_part_color(p->type, p->subtype, isRunning);
            graphEntries[graphCount].isRunning = isRunning;

            bool isSpiffs = (p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS ||
                             p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT);
            if (isSpiffs && littlefs_is_mounted() && littlefs_total_bytes() > 0) {
                graphEntries[graphCount].usedFrac =
                    (float)littlefs_used_bytes() / (float)littlefs_total_bytes();
            } else {
                graphEntries[graphCount].usedFrac = -1.0f;
            }
            graphCount++;
        }

        it = esp_partition_next(it);   // libère l'itérateur en interne au dernier appel
    }

    // --- Graphe : barre empilée = flash entière ---
    int gy = SI_H - 14 - legendH - 4 - graphH;
    int gx = SI_LX;
    int gw = SI_W - SI_LX * 2;

    _si_sprite.drawRect(gx, gy, gw, graphH, C_DIM);

    uint32_t cursorAddr = 0;
    int      cursorX    = gx + 1;
    int      barInnerW  = gw - 2;

    for (int i = 0; i < graphCount; i++) {
        // Espace non alloué avant cette partition (bootloader, table de part.)
        if (graphEntries[i].address > cursorAddr) {
            uint32_t gap = graphEntries[i].address - cursorAddr;
            int gapW = (int)((uint64_t)gap * barInnerW / flashTotal);
            if (gapW > 0) {
                _si_sprite.fillRect(cursorX, gy + 1, gapW, graphH - 2, C_GRID);
                cursorX += gapW;
            }
        }

        int segW = (int)((uint64_t)graphEntries[i].size * barInnerW / flashTotal);
        if (segW < 1) segW = 1;
        _si_sprite.fillRect(cursorX, gy + 1, segW, graphH - 2, graphEntries[i].color);

        // Surimpression : portion réellement occupée (spiffs/fat, réservé large)
        if (graphEntries[i].usedFrac >= 0.0f) {
            int usedW = (int)(segW * constrain(graphEntries[i].usedFrac, 0.0f, 1.0f));
            if (usedW > 0) _si_sprite.fillRect(cursorX, gy + 1, usedW, graphH - 2, C_MAGENTA_DK);
        }

        // Délimiteur sur CHAQUE segment (blanc pour la partition active) : sans
        // ça, deux partitions de même couleur se lisaient comme un seul bloc.
        _si_sprite.drawRect(cursorX, gy + 1, segW, graphH - 2,
                            graphEntries[i].isRunning ? C_WHITE : C_DKCYAN);

        cursorX += segW;
        cursorAddr = graphEntries[i].address + graphEntries[i].size;
    }

    if (cursorX < gx + 1 + barInnerW) {
        _si_sprite.fillRect(cursorX, gy + 1, gx + 1 + barInnerW - cursorX, graphH - 2, C_GRID);
    }

    // --- Légende ---
    int ly = gy + graphH + 4;
    int lx = gx;
    const int swatch = 8;

    auto drawLegendItem = [&](uint16_t color, const char* label) {
        if (lx + swatch + (int)strlen(label) * 6 + 10 > SI_W - SI_LX) {
            lx = gx;
            ly += 9;
        }
        _si_sprite.fillRect(lx, ly, swatch, swatch, color);
        _si_sprite.drawRect(lx, ly, swatch, swatch, C_DIM);
        _si_sprite.setTextColor(C_DIM, C_BG);
        _si_sprite.setCursor(lx + swatch + 2, ly);
        _si_sprite.print(label);
        lx += swatch + (int)strlen(label) * 6 + 10;
    };

    drawLegendItem(C_YELLOW, "nvs");
    drawLegendItem(C_DKCYAN, "app inactif");
    drawLegendItem(C_GREEN, "app actif");
    drawLegendItem(C_MAGENTA, "spiffs/fat");
    drawLegendItem(C_GRID, "libre/system");
}

// --- PAGE 6 — LITTLEFS ---
// Sous-répertoires affichés en une ligne agrégée "[DIR] nom (N)". Pattern
// d'itération conforme à l'exemple officiel listDir() : openNextFile() est
// TOUJOURS rappelé sur le handle du répertoire, jamais sur l'entrée renvoyée —
// et un sous-répertoire doit être rouvert par chemin pour obtenir son propre
// curseur. Pas de récursion plus profonde : la hiérarchie est plate.
static void _si_page_fs() {
    char buf[48];
    int y = 30;

    _si_section(y, ">> LITTLEFS"); y += SI_LH;

    if (!littlefs_is_mounted()) {
        _si_row(y, "  Etat        : ", "NON MONTE", C_RED);
        return;
    }

    size_t total = littlefs_total_bytes();
    size_t used  = littlefs_used_bytes();
    snprintf(buf, sizeof(buf), "%lu KB / %lu KB",
             (unsigned long)(used / 1024), (unsigned long)(total / 1024));
    _si_row(y, "  Occupation  : ", buf, C_YELLOW); y += SI_LH;

    _si_hline(y); y += 5;

    _si_sprite.setTextColor(C_DKCYAN, C_BG);
    _si_sprite.setTextSize(1);
    _si_sprite.setCursor(SI_LX, y);  _si_sprite.print("FICHIER");
    _si_sprite.setCursor(220, y);    _si_sprite.print("TAILLE");
    y += SI_LH;

    fs::File root = littlefs_open("/", "r");
    if (!root || !root.isDirectory()) {
        _si_row(y, "  Erreur      : ", "racine invalide", C_RED);
        return;
    }

    const int footerBlock = SI_LH * 2 + 8;   // 2 lignes de résumé + séparateur
    const int maxY = SI_H - 14 - footerBlock;

    uint16_t fileCount = 0;   // fichiers réels, sous-répertoires inclus
    fs::File file = root.openNextFile();

    while (file && y < maxY) {
        char name[40];

        if (file.isDirectory()) {
            uint16_t subCount = 0;
            size_t   subSize  = 0;

            fs::File subDir = littlefs_open(file.path(), "r");
            if (subDir) {
                fs::File sub = subDir.openNextFile();
                while (sub) {
                    subCount++;
                    subSize += sub.size();
                    sub.close();
                    sub = subDir.openNextFile();
                }
                subDir.close();
            }

            snprintf(name, sizeof(name), "[DIR] %.16s (%u)", file.name(), subCount);
            _si_sprite.setTextColor(C_MEM_EXT, C_BG);   // bleu : distingue les répertoires des fichiers
            _si_sprite.setCursor(SI_LX, y);
            _si_sprite.print(name);

            snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)((subSize + 1023) / 1024));
            _si_sprite.setTextColor(C_GREEN, C_BG);
            _si_sprite.setCursor(220, y);
            _si_sprite.print(buf);

            fileCount += subCount;
        } else {
            snprintf(name, sizeof(name), "%.20s", file.name());
            _si_sprite.setTextColor(C_WHITE, C_BG);
            _si_sprite.setCursor(SI_LX, y);
            _si_sprite.print(name);

            snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)((file.size() + 1023) / 1024));
            _si_sprite.setTextColor(C_GREEN, C_BG);
            _si_sprite.setCursor(220, y);
            _si_sprite.print(buf);

            fileCount++;
        }

        y += SI_LH;

        fs::File next = root.openNextFile();
        file.close();
        file = next;
    }

    if (file) {
        _si_sprite.setTextColor(C_ORANGE, C_BG);
        _si_sprite.setCursor(SI_LX, y);
        _si_sprite.print("... suite ...");
        y += SI_LH;
        file.close();
    }
    root.close();

    _si_hline(y); y += 4;

    snprintf(buf, sizeof(buf), "%u fichier(s)  |  Libre : %lu KB",
             fileCount, (unsigned long)((total - used) / 1024));
    _si_sprite.setTextColor(C_CYAN, C_BG);
    _si_sprite.setCursor(SI_LX, y);
    _si_sprite.print(buf);
}

// --- Dispatcher ---

static void _si_render_page(int page) {
    // Seule la page TÂCHES le renseignera : point de passage commun à toutes
    // les navigations, donc le seul endroit où le remettre à zéro.
    _si_task_shown = 0;
    switch (page) {
        case SI_PAGE_CHIP:  _si_page_chip();       break;
        case SI_PAGE_MEM:   _si_page_mem();        break;
        case SI_PAGE_NET:   _si_page_net();        break;
        case SI_PAGE_TASKS: _si_page_tasks();      break;
        case SI_PAGE_PART:  _si_page_partitions(); break;
        case SI_PAGE_FS:    _si_page_fs();         break;
    }
}

// --- Intégration LVGL (sprite hors-écran → canvas) ---

static void _si_full_redraw(int page) {
    _si_fill_bg();
    _si_header(page);
    _si_footer();
    _si_clear_content();
    _si_render_page(page);
    _si_blit();
}

static void _si_redraw_page(int page) {
    _si_header(page);
    _si_clear_content();
    _si_render_page(page);
    _si_blit();
}

// Détruit le timer d'horodatage. ⚠️ Doit être appelé sur TOUTE sortie de
// l'écran, pas seulement par _si_hide : display_show_home/nas/ai() font un
// lv_scr_load() nu, et une détection "Jarvis" bascule aussi sur l'écran IA.
// Un timer survivant continuait de blitter dans un canvas invisible, ET un
// second était créé au retour — deux timers font avancer le compteur de ticks
// deux fois trop vite, ce qui a faussé trois campagnes de mesure d'affilée.
static void _si_stop_timer() {
    if (_si_uptime_timer) {
        lv_timer_del(_si_uptime_timer);
        _si_uptime_timer = nullptr;
    }
}

static void _si_screen_unloaded_cb(lv_event_t* e) {
    _si_stop_timer();
}

static void _si_hide() {
    _si_stop_timer();
    _si_task_shown = 0;
    if (_si_return_screen) lv_scr_load(_si_return_screen);
}

// Horodatage à chaque tick, puis valeurs vivantes une fois par seconde. Tourne
// via un lv_timer, donc sur loopTask. Blits partiels pour ne pas saturer le SPI.
static void _si_uptime_timer_cb(lv_timer_t* t) {
    _si_uptime();
    _si_blit_rect(SI_W - 64, 0, 64, 26);

    static uint8_t ticks = 0;
    if (++ticks < SI_REFRESH_TICKS) return;
    ticks = 0;

    switch (_si_page) {
        case SI_PAGE_CHIP:  _si_refresh_chip();  break;

        // MEMOIRE est la page la plus chère (~250 ms : gros bloc, et le canvas
        // est relu depuis la PSRAM). Le heap ne bouge pas assez vite pour
        // justifier 1 Hz — on l'espace, c'est le seul levier qui n'ait pas
        // empiré les choses (cf. l'avertissement sur _si_refresh_mem).
        case SI_PAGE_MEM: {
            static uint8_t skip = 0;
            if (++skip >= SI_MEM_EVERY_N) { skip = 0; _si_refresh_mem(); }
            break;
        }

        case SI_PAGE_TASKS: _si_refresh_tasks(); break;
        default: break;   // NET / PART / FS : rien ne bouge
    }
}

static void _si_next_page() {
    _si_page = (_si_page + 1) % SI_PAGE_COUNT;
    _si_redraw_page(_si_page);
}

// Tap : gauche = page précédente, droite = suivante, centre = sortie.
// LVGL a déjà résolu l'appui/relâchement en un CLICKED unique, pas de débounce.
static void _si_canvas_click_cb(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (p.x < 110) {
        _si_page = (_si_page - 1 + SI_PAGE_COUNT) % SI_PAGE_COUNT;
        _si_redraw_page(_si_page);
    } else if (p.x > 210) {
        _si_next_page();
    } else {
        _si_hide();
    }
}

// Créé une seule fois, à la première ouverture de l'écran.
static void _si_ensure_created() {
    if (_si_screen) return;

    _si_sprite.setColorDepth(16);
    _si_sprite.createSprite(SI_W, SI_H);   // ~150 Ko, en PSRAM automatiquement

    _si_canvas_buf = (lv_color_t*)heap_caps_malloc((size_t)SI_W * SI_H * 2, MALLOC_CAP_SPIRAM);
    if (!_si_canvas_buf) {
        // Sans buffer, lv_canvas_set_buffer et les memcpy de _si_blit
        // déréférenceraient nullptr : on renonce à l'écran plutôt que planter.
        log_line("[SysInfo] PSRAM KO pour le canvas (%u o) — ecran indisponible",
                 (unsigned)((size_t)SI_W * SI_H * 2));
        _si_sprite.deleteSprite();
        return;
    }

    _si_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_si_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(_si_screen, 0, 0);
    lv_obj_set_style_border_width(_si_screen, 0, 0);

    // Filet couvrant TOUTES les sorties d'écran, y compris celles qui ne
    // passent pas par _si_hide (page:home, bascule sur l'écran IA au réveil).
    lv_obj_add_event_cb(_si_screen, _si_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, nullptr);

    _si_canvas = lv_canvas_create(_si_screen);
    lv_canvas_set_buffer(_si_canvas, _si_canvas_buf, SI_W, SI_H, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_obj_set_pos(_si_canvas, 0, 0);
    lv_obj_add_flag(_si_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_si_canvas, _si_canvas_click_cb, LV_EVENT_CLICKED, nullptr);
}

// ---- API PUBLIQUES ----

// Séparateur titré à largeur fixe : "=== TITRE ===…===(NN% libre)".
static void _si_mem_sep(const char* title, unsigned pctFree) {
    const int W = 45;
    char line[W + 1];
    char suffix[16];
    int slen = snprintf(suffix, sizeof(suffix), "(%u%% libre)", pctFree);
    int plen = snprintf(line, sizeof(line), "=== %s ", title);   // "=== RAM " + '\0'
    if (plen < 0 || plen > W) plen = 0;
    for (int i = plen; i < W - slen; i++) line[i] = '=';
    if (slen > 0 && slen <= W) memcpy(line + (W - slen), suffix, slen);
    line[W] = '\0';
    log_line("[MEM] %s", line);
}

// Séparateur plein (fermeture).
static void _si_mem_sep_plain() {
    char line[46];
    memset(line, '=', 45);
    line[45] = '\0';
    log_line("[MEM] %s", line);
}

// Journalise l'état mémoire (cmd "mem"). Volontairement en octets : le pire cas
// interne se joue à quelques Ko près, l'arrondi Ko de la page MÉMOIRE est trop
// grossier pour le suivi. Trois sections : RAM (postes internes → bilan → marges),
// PSRAM (postes → bilan → marge), puis le plus gros bloc contigu.
void sysinfo_log_memory() {
    size_t   intFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t   intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t   intMin   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t   intUsed  = intTotal - intFree;
    unsigned intPct   = intTotal ? (unsigned)(100u * intFree / intTotal) : 0;

    uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t dmaMin  = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);

    bool     hasPs   = psramFound();
    uint32_t psTotal = hasPs ? ESP.getPsramSize() : 0;
    uint32_t psFree  = hasPs ? ESP.getFreePsram() : 0;
    uint32_t psMin   = hasPs ? heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) : 0;
    unsigned psPct   = psTotal ? (unsigned)(100u * psFree / psTotal) : 0;

    // Comptabilité interne : intUsed = postes connus (_si_alloc internes + les 6
    // piles _si_stacks) + ESP_SR (mesuré à part) + reste (non tracé : WiFi/lwIP,
    // cœur, FreeRTOS…). Reste SIGNÉ : un négatif signalerait que la table sur-compte.
    uint32_t knownInt = 0;
    for (auto const& a : _si_alloc)  if (a.internal) knownInt += a.bytes;
    for (auto const& s : _si_stacks)                 knownInt += s.stack;
    uint32_t espSr = wakeword_esp_sr_internal_bytes();
    long     reste = (long)intUsed - (long)knownInt - (long)espSr;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);

    // === RAM (interne) : postes → bilan → marges ===
    _si_mem_sep("RAM", intPct);
    if (n == 0) {
        log_line("[MEM] %-20s : uxTaskGetSystemState a echoue", "Piles");
    } else {
        // High-water : le plus petit reste JAMAIS atteint depuis le boot (pas le
        // libre courant) — un min bas = le pic a DÉJÀ frôlé le débordement.
        for (auto const& s : _si_stacks) {
            for (UBaseType_t i = 0; i < n; i++) {
                if (strcmp(st[i].pcTaskName, s.name) != 0) continue;
                unsigned freeBytes = (unsigned)st[i].usStackHighWaterMark * sizeof(StackType_t);
                unsigned pct = 100u * freeBytes / s.stack;
                log_line("[MEM] Pile %-15s : min %u / %u o (%u%% libre)%s",
                         s.name, freeBytes, s.stack, pct,
                         freeBytes < 1024 ? "  <<< MARGE FAIBLE" : "");
                break;
            }
        }
    }
    for (auto const& a : _si_alloc)
        if (a.internal) log_line("[MEM] Buf. %-15s : %u o", a.label, (unsigned)a.bytes);
    log_line("[MEM] %-20s : %u o d'interne (mesure au boot)", "ESP_SR occupe", (unsigned)espSr);
    log_line("[MEM] %-20s : %ld o", "Systeme (non trace)", reste);
    log_line("[MEM] %-20s : %u o utilise / %u o", "   Bilan HEAP", (unsigned)intUsed, (unsigned)intTotal);
    log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "Interne", (unsigned)intFree, (unsigned)intMin);
    log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "DMA", (unsigned)dmaFree, (unsigned)dmaMin);
    // Un total libre confortable ne garantit pas qu'une alloc d'un seul tenant passe.
    log_line("[MEM] %-20s : %u o", "Plus gros bloc libre",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // === PSRAM : postes → bilan → marge ===
    if (hasPs) {
        _si_mem_sep("PSRAM", psPct);
        uint32_t knownPs = 0;
        for (auto const& a : _si_alloc) {
            if (a.internal) continue;
            uint32_t bytes = a.bytes;
            // Sprite/Canvas SysInfo : alloués seulement à la 1re ouverture — gatés
            // pour que le reste PSRAM reste exact dans les deux états.
            if (!_si_screen && (strcmp(a.label, "Sprite") == 0 || strcmp(a.label, "Canvas") == 0))
                bytes = 0;
            knownPs += bytes;
            log_line("[MEM] Buf. %-15s : %u o", a.label, (unsigned)bytes);
        }
        // Non tracé : modèles ESP_SR chargés en PSRAM + framework.
        log_line("[MEM] %-20s : %ld o", "Systeme (non trace)", (long)(psTotal - psFree) - (long)knownPs);
        log_line("[MEM] %-20s : %u o utilise / %u o", "   Bilan PSRAM", (unsigned)(psTotal - psFree), (unsigned)psTotal);
        log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "PSRAM", (unsigned)psFree, (unsigned)psMin);
        log_line("[MEM] %-20s : %u o", "Plus gros bloc libre",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    } else {
        _si_mem_sep("PSRAM", 0);
        log_line("[MEM] %-20s : NON DETECTEE", "PSRAM");
    }

    _si_mem_sep_plain();   // ferme le dernier bloc
}

void display_show_sysinfo() {
    if (_si_screen && lv_scr_act() == _si_screen) {
        // Déjà affiché (rappel via esp32/cmd ou POST /cmd) : le bouton sert
        // alors à parcourir les pages, faute d'écran tactile distant.
        _si_next_page();
        return;
    }
    display_show_sysinfo_page(SI_PAGE_CHIP);
}

void display_show_sysinfo_page(int page) {
    if (page < 0 || page >= SI_PAGE_COUNT) {
        log_line("[SysInfo] page hors plage : %d (0-%d)", page, SI_PAGE_COUNT - 1);
        return;
    }

    if (_si_screen && lv_scr_act() == _si_screen) {
        // Écran déjà en place : on ne fait que changer de page.
        if (page != _si_page) {
            _si_page = page;
            _si_redraw_page(_si_page);
        }
        return;
    }

    _si_ensure_created();
    if (!_si_screen) return;   // allocation PSRAM échouée

    _si_return_screen = lv_scr_act();
    _si_page = page;

    lv_scr_load(_si_screen);
    _si_full_redraw(_si_page);

    _si_stop_timer();   // idempotent : jamais deux timers, quoi qu'il arrive
    _si_uptime_timer = lv_timer_create(_si_uptime_timer_cb, 50, nullptr);
}
