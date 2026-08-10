// ============================================================
// SYSINFO_MANAGER.CPP — écran de diagnostic système.
// Dessiné par gfx dans un buffer hors-écran puis copié (memcpy) dans un
// lv_canvas affiché comme un écran LVGL normal. 7 pages, navigation
// tactile gauche/droite/centre.
//
// ORGANISATION DU FICHIER
//   1. Les OBJETS      — palette, géométrie, surface, dessin, cadre, format,
//                        cpu, inventaire. Aucun ne connaît les pages.
//   2. Les PAGES       — un bloc autonome chacun, découpé en trois :
//                          FIXE       dessiné une fois, à l'entrée sur la page
//                          BLITTÉ     réécrit par-dessus à chaque rafraîchissement
//                          ASSEMBLAGE ce que le dispatcher appelle
//   3. La TABLE        — _pages[] : draw / refresh / période en ticks de 50 ms.
//                        Ajouter une page = une ligne + un bloc.
//   4. Écran, ticker, API publiques.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
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
#include <esp_app_format.h>   // en-tête d'image ESP — occupation d'un slot OTA

// ---- RESSOURCES LOCALES ----
#include "sysinfo_manager.h"
#include "display_gfx.h"
#include "display_driver.h"
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "wakeword_manager.h"
#include "light_manager.h"
#include "littlefs_manager.h"
#include "log_manager.h"
// Pour les tailles de l'inventaire — chaque poste vient du header de son propriétaire
#include "display_manager.h"
#include "audio_manager.h"
#include "ai_companion.h"


// ════════════════════════════════════════════════════════════
// PALETTE
// ════════════════════════════════════════════════════════════

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

// Localisation mémoire (page MEMOIRE) : OÙ vit une allocation, un seul axe —
// pas de rampe de criticité, orange ne doit pas signifier "interne" ET "alarme".
#define C_MEM_INT     C_ORANGE   // RAM interne
#define C_MEM_EXT     0x64BD     // PSRAM (externe) — bleu bleuet
// Teintes sourdes des deux mêmes couleurs : la LUMINOSITÉ hiérarchise, la
// teinte reste la localisation.
#define C_MEM_INT_DK  0x7A80
#define C_MEM_EXT_DK  0x324E


// ════════════════════════════════════════════════════════════
// GÉOMÉTRIE
// ════════════════════════════════════════════════════════════

#define SI_W   320
#define SI_H   240
#define SI_LH  12     // hauteur de ligne
#define SI_LX  8      // marge gauche

#define SI_HEADER_H   16
#define SI_FOOTER_H   14
#define SI_CONTENT_Y  (SI_HEADER_H + 1)
#define SI_CONTENT_H  (SI_H - SI_CONTENT_Y - SI_FOOTER_H)
#define SI_PAGE_Y0    (SI_CONTENT_Y + 3)   // 1re ligne utile d'une page
#define SI_PAGE_YMAX  (SI_H - SI_FOOTER_H) // 1re ligne INTERDITE (pied de page)

#define SI_CLOCK_W    64    // largeur du coin horodatage (blit le plus fréquent)

#define SI_MAX_TASKS  32

#define SI_REFRESH_TICKS  20    // timer à 50 ms -> rafraîchissement 1 Hz
#define SI_SENSOR_TICKS   2     // page CAPTEUR : 10 Hz, on y suit la main à vue


// ════════════════════════════════════════════════════════════
// SURFACE — sprite hors-écran + recopie vers le canvas LVGL
// ════════════════════════════════════════════════════════════

namespace surface {

static uint16_t*   buf        = nullptr; // surface de dessin hors-écran (PSRAM)
static gfx::Canvas cv         = { nullptr, SI_W, SI_H };
static lv_obj_t*   canvas     = nullptr; // canvas plein écran
static lv_color_t* canvas_buf = nullptr; // buffer PSRAM du canvas

// Primitives de la surface — gfx sans avoir à répéter la cible.
static inline void fill(uint16_t c)                                { gfx::fill(cv, c); }
static inline void fill_rect(int x, int y, int w, int h, uint16_t c) { gfx::fill_rect(cv, x, y, w, h, c); }
static inline void rect(int x, int y, int w, int h, uint16_t c)      { gfx::rect(cv, x, y, w, h, c); }
static inline void hline(int x, int y, int w, uint16_t c)            { gfx::hline(cv, x, y, w, c); }
static inline void vline(int x, int y, int h, uint16_t c)            { gfx::vline(cv, x, y, h, c); }
static inline void pixel(int x, int y, uint16_t c)                   { gfx::pixel(cv, x, y, c); }
static inline void text(int x, int y, const char* s, uint16_t fg, uint16_t bg, uint8_t sz = 1) {
    gfx::text(cv, x, y, s, fg, bg, sz);
}

// Surface -> canvas. Même format des deux côtés (RGB565 ordre dalle) :
// simple memcpy, aucune conversion.
static void blit() {
    if (!canvas_buf) return;
    memcpy(canvas_buf, buf, (size_t)SI_W * SI_H * 2);
    lv_obj_invalidate(canvas);
}

// Ne recopie qu'un rectangle plutôt que le canvas entier.
// ⚠️ Découper en HAUTEUR (rangées pleine largeur, contiguës en PSRAM) est le
// seul découpage rentable : rogner la LARGEUR ne rend presque rien, l'accès
// PSRAM domine tout le reste.
static void blit_rect(int x, int y, int w, int h) {
    if (!canvas_buf) return;
    const uint8_t* srcBase = (const uint8_t*)buf;
    uint8_t*       dstBase = (uint8_t*)canvas_buf;
    for (int row = 0; row < h; row++) {
        size_t offset = (size_t)((y + row) * SI_W + x) * 2;
        memcpy(dstBase + offset, srcBase + offset, (size_t)w * 2);
    }
    lv_area_t area = { x, y, x + w - 1, y + h - 1 };
    lv_obj_invalidate_area(canvas, &area);
}

// Rangée(s) pleine largeur — la forme de blit partiel à privilégier.
static void blit_rows(int y, int h) { blit_rect(0, y, SI_W, h); }

}  // namespace surface


// ════════════════════════════════════════════════════════════
// DRAW — primitives de dessin, famille UNIQUE
// ════════════════════════════════════════════════════════════

namespace draw {

static void fill_bg() {
    surface::fill(C_BG);
    for (int x = 0; x < SI_W; x += 16)
        for (int y = 0; y < SI_H; y += 16)
            surface::pixel(x, y, C_GRID);
}

static void hline(int y, uint16_t c = C_DIM) {
    surface::hline(SI_LX, y, SI_W - SI_LX * 2, c);
}

// Titre de section : ">> NOM"
static void section(int y, const char* title) {
    surface::text(SI_LX, y, title, C_CYAN, C_BG);
}

// Trois hauteurs, trois rôles — et rien d'autre.
constexpr int BAR_ROW  = 7;    // jauge dans une ligne de table
constexpr int BAR_HERO = 12;   // jauge du bandeau vedette, UNE par page
constexpr int RULE     = 2;    // filet de proportion sous une ligne de liste

static void bar(int x, int y, int w, int h, float pct, uint16_t c) {
    surface::rect(x, y, w, h, C_DIM);
    int fill = (int)((w - 2) * constrain(pct, 0.0f, 1.0f));
    surface::fill_rect(x + 1,        y + 1, fill,         h - 2, c);
    surface::fill_rect(x + 1 + fill, y + 1, w - 2 - fill, h - 2, C_BG);
}

// "clé : valeur" posé à un x libre, sans barre — forme compacte, pour les
// mises en page à deux colonnes.
static void pair(int x, int y, const char* key, const char* val,
                 uint16_t valColor = C_WHITE) {
    surface::text(x, y, key, C_DKCYAN, C_BG);
    surface::text(x + gfx::text_w(key), y, val, valColor, C_BG);
}

// Ligne "clé : valeur" pleine largeur, avec barre optionnelle.
// barMaxW : largeur max de la barre en px, 0 = jusqu'à la marge droite.
static void row(int y, const char* key, const char* val,
                uint16_t valColor = C_GREEN, bool withBar = false,
                float barPct = 0, uint16_t barColor = C_CYAN,
                int barMaxW = 0) {
    surface::text(SI_LX, y, key, C_DKCYAN, C_BG);

    int vx = SI_LX + gfx::text_w(key) + 6;
    surface::text(vx, y, val, valColor, C_BG);

    if (withBar) {
        int bx = vx + gfx::text_w(val) + 4;
        int bw = SI_W - bx - SI_LX;
        if (barMaxW > 0 && bw > barMaxW) bw = barMaxW;
        if (bw > 10) bar(bx, y + 1, bw, BAR_ROW, barPct, barColor);
    }
}

// Texte brut à une position — pour les tableaux, où les intitulés sont en tête
// de colonne. bg : fond peint sous les glyphes, à passer dès qu'on écrit sur
// autre chose que le fond de l'écran.
static void text(int x, int y, const char* s, uint16_t c, uint16_t bg = C_BG) {
    surface::text(x, y, s, c, bg);
}

// Pastille pleine — un état court qu'on doit voir avant de lire (partition
// active, système non monté). Retourne sa largeur, pour en enchaîner plusieurs.
static int badge(int x, int y, const char* s, uint16_t bg, uint16_t fg = C_BG) {
    int w = gfx::text_w(s) + 6;
    surface::fill_rect(x, y - 1, w, 10, bg);
    surface::text(x + 3, y, s, fg, bg);
    return w;
}

// Double taille (12x16) — réservé aux valeurs « hero » de la page IDENTITE.
static void big(int x, int y, const char* s, uint16_t c, uint16_t bg = C_BG) {
    surface::text(x, y, s, c, bg, 2);
}

// Aligné à DROITE sur xr (1re colonne interdite). Police fixe 6 px.
static void text_right(int xr, int y, const char* s, uint16_t c, uint16_t bg = C_BG) {
    text(xr - gfx::text_w(s), y, s, c, bg);
}

// Efface une bande pleine largeur — préalable à tout redessin de zone BLITTÉE.
static void wipe_rows(int y, int h) {
    surface::fill_rect(0, y, SI_W, h, C_BG);
}

}  // namespace draw


// ════════════════════════════════════════════════════════════
// FRAME — bandeau, pied de page, horodatage, zone de contenu
// ════════════════════════════════════════════════════════════

namespace frame {

static void header(int page) {
    surface::fill_rect(0, 0, SI_W, SI_HEADER_H, C_BG);
    surface::hline(0, 0, SI_W, C_CYAN);
    surface::hline(0, 1, SI_W, C_DKCYAN);

    char buf[16];
    snprintf(buf, sizeof(buf), "PG %d/%d", page + 1, SYSINFO_PAGE_COUNT);
    draw::text(SI_LX, 4, buf, C_YELLOW);

    const char* title = "[ ES3C28P SYSTEM DIAGNOSTICS ]";
    draw::text((SI_W - gfx::text_w(title)) / 2, 4, title, C_CYAN);

    surface::hline(0, SI_HEADER_H - 2, SI_W, C_DKCYAN);
    surface::hline(0, SI_HEADER_H - 1, SI_W, C_CYAN);
}

static void footer() {
    surface::hline(0, SI_H - 13, SI_W, C_DKCYAN);
    surface::hline(0, SI_H - 12, SI_W, C_CYAN);

    draw::text(SI_LX, SI_H - 9, "< PREV", C_DIM);

    const char* mid = "TAP CENTER = EXIT";
    draw::text((SI_W - gfx::text_w(mid)) / 2, SI_H - 9, mid, C_DIM);

    const char* right = "NEXT >";
    draw::text(SI_W - SI_LX - gfx::text_w(right), SI_H - 9, right, C_DIM);
}

static void clear_content() {
    surface::fill_rect(0, SI_CONTENT_Y, SI_W, SI_CONTENT_H, C_BG);
}

// Horodatage mm:ss.mmm, réécrit à 20 Hz — environ 12 % de loopTask sur toutes
// les pages SysInfo.
static void clock() {
    unsigned long ms = millis();
    unsigned long s  = ms / 1000;
    unsigned long m  = s / 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu.%03lu", m, s % 60, ms % 1000);
    surface::fill_rect(SI_W - SI_CLOCK_W, 2, SI_CLOCK_W, SI_HEADER_H - 4, C_BG);
    draw::text(SI_W - SI_CLOCK_W, 4, buf, C_YELLOW);
}

static void blit_clock() {
    surface::blit_rect(SI_W - SI_CLOCK_W, 0, SI_CLOCK_W, SI_HEADER_H);
}

}  // namespace frame


// ════════════════════════════════════════════════════════════
// FMT — énumérations matérielles et tailles vers texte
// ════════════════════════════════════════════════════════════

namespace fmt {

// Sous le kilo-octet, afficher en octets — sinon un poste de 256 o lit "0 KB".
static void size(char* buf, size_t n, long bytes) {
    if (bytes > -1024 && bytes < 1024) snprintf(buf, n, "%ld o",  bytes);
    else                               snprintf(buf, n, "%ld KB", bytes / 1024);
}

static const char* chip_model(esp_chip_model_t m) {
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

static const char* flash_mode(FlashMode_t m) {
    switch (m) {
        case FM_QIO:  return "QIO";
        case FM_QOUT: return "QOUT";
        case FM_DIO:  return "DIO";
        case FM_DOUT: return "DOUT";
        default:      return "?";
    }
}

static const char* reset_reason(esp_reset_reason_t r) {
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

static const char* part_subtype(uint8_t type, uint8_t subtype) {
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

static char task_state(eTaskState s) {
    switch (s) {
        case eRunning:   return 'X';
        case eReady:     return 'R';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

// Rang de tri : running > ready > blocked > suspended.
static uint8_t task_rank(eTaskState s) {
    switch (s) {
        case eRunning:   return 0;
        case eReady:     return 1;
        case eBlocked:   return 2;
        case eSuspended: return 3;
        case eDeleted:   return 4;
        default:         return 5;
    }
}

}  // namespace fmt


// ════════════════════════════════════════════════════════════
// CPU — %CPU par tâche et par cœur, mesuré en DELTA
// ════════════════════════════════════════════════════════════

namespace cpu {

// Snapshot précédent des compteurs d'exécution FreeRTOS.
// ⚠️ Le compteur est un u32 de MICROSECONDES : il déborde toutes les 71,6 min,
// donc mesure en DELTA obligatoire. La soustraction non signée reste juste tant
// que deux relevés sont plus rapprochés que ça.
static struct { TaskHandle_t h; uint32_t run; } _prev[SI_MAX_TASKS];
static UBaseType_t _prev_n  = 0;
static uint32_t    _prev_us = 0;

// Échantillonne les compteurs d'exécution et met à jour le snapshot.
//   pct  (optionnel) : rempli avec le %CPU de chaque tâche de st[]
//   core0/core1      : charge de chaque cœur, déduite du temps des tâches IDLE
// Retourne false si la fenêtre de mesure n'est pas exploitable (premier relevé
// au-delà du débordement du compteur, ou fenêtre trop courte).
static bool sample(TaskStatus_t* st, UBaseType_t n, uint8_t* pct,
                   int* core0, int* core1) {
    uint32_t now_us = (uint32_t)esp_timer_get_time();   // même troncature u32 que le compteur
    bool     first  = (_prev_n == 0);
    uint32_t dt_us  = first ? now_us : (now_us - _prev_us);

    // Premier relevé : pas de fenêtre exploitable.
    bool valid = (dt_us > 1000) && !(first && millis() > 71UL * 60 * 1000);

    uint32_t idle_us[2] = { 0, 0 };

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t prev = 0;
        for (UBaseType_t j = 0; j < _prev_n; j++) {
            if (_prev[j].h == st[i].xHandle) { prev = _prev[j].run; break; }
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
    _prev_n = (n < SI_MAX_TASKS) ? n : SI_MAX_TASKS;
    for (UBaseType_t i = 0; i < _prev_n; i++) {
        _prev[i].h   = st[i].xHandle;
        _prev[i].run = st[i].ulRunTimeCounter;
    }
    _prev_us = now_us;

    return valid;
}

// Charge des deux cœurs seule (page IDENTITE).
static bool load(int* core0, int* core1) {
    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);
    if (n == 0) { *core0 = *core1 = -1; return false; }
    return sample(st, n, nullptr, core0, core1);
}

}  // namespace cpu


// ════════════════════════════════════════════════════════════
// INV — inventaire mémoire : postes connus, piles, comptabilité
// ════════════════════════════════════════════════════════════
// Source unique des tailles affichées par la page MEMOIRE ET journalisées par
// sysinfo_log_memory(). Chaque poste vient du header de son propriétaire.

namespace inv {

// dyn : poste dont la taille n'est connue qu'au runtime. Laissé à nullptr par
// l'init agrégée pour tous les autres. ⚠️ Lire par size(), jamais par `bytes`.
struct Alloc {
    const char* label; bool internal; uint32_t bytes; uint32_t (*dyn)();
    uint32_t size() const { return dyn ? dyn() : bytes; }
};

static const Alloc allocs[] = {
    { "LVGL",       true,  LV_BUF_BYTES * LV_BUF_N },
    { "MQTT out",   true,  MQTT_OUT_BUFFER_SIZE },
    { "MQTT in",    false, MQTT_BUFFER_SIZE },
    { "MQTT build", false, MQTT_BUFFER_SIZE },
    { "Capture",    false, AUDIO_RECORD_CAPACITY_SAMPLES * sizeof(int16_t) },
    { "Flux TTS",   false, AUDIO_STREAM_BYTES },
    { "Avatar",     false, 0, ai_companion_psram_bytes },
    { "JSON",       false, JSON_TOTAL_SIZE },
    { "Sprite",     false, (uint32_t)SI_W * SI_H * 2 },
    { "Canvas",     false, (uint32_t)SI_W * SI_H * 2 },
    { "Screenshot", false, (uint32_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2 },
};
static const int alloc_count = sizeof(allocs) / sizeof(allocs[0]);

// ⚠️ Miroir des tailles de config.h : TaskStatus_t ne donne que le high-water,
// jamais la taille allouée — sans ce lookup, pas de pourcentage.
struct Stack { const char* name; uint16_t stack; };

static const Stack stacks[] = {
    { "loopTask",   STACK_BYTES_LOOP_TASK  },
    { "audio_task", STACK_BYTES_AUDIO_TASK },
    { "ai_task",    STACK_BYTES_AI_TASK    },
    { "http_task",  STACK_BYTES_HTTP_TASK  },
    { "mqtt_task",  STACK_BYTES_MQTT_TASK  },
    { "ota_task",   STACK_BYTES_OTA_TASK   },
};
static const int stack_count = sizeof(stacks) / sizeof(stacks[0]);

// Taille de pile allouée, 0 si inconnue (tâche système)
static uint16_t stack_size(const char* name) {
    for (auto const& s : stacks)
        if (strcmp(s.name, name) == 0) return s.stack;
    return 0;
}

// Buffers alloués paresseusement : tant qu'ils n'existent pas, les compter
// fausserait le "non tracé" PSRAM d'autant.
static bool sysinfo_allocated() { return surface::canvas_buf != nullptr; }

// Un seul endroit décide, sinon le total et le journal divergent.
static bool alloc_pending(const Alloc& a) {
    if (strcmp(a.label, "Sprite") == 0 || strcmp(a.label, "Canvas") == 0)
        return !sysinfo_allocated();
    if (strcmp(a.label, "Screenshot") == 0) return !panel_capture_allocated();
    if (strcmp(a.label, "Flux TTS")   == 0) return !audio_stream_allocated();
    return false;
}

static uint32_t stacks_bytes() {
    uint32_t sum = 0;
    for (auto const& s : stacks) sum += s.stack;
    return sum;
}

static uint32_t known_internal() {
    uint32_t sum = stacks_bytes();
    for (auto const& a : allocs) if (a.internal) sum += a.size();
    return sum;
}

static uint32_t known_psram() {
    uint32_t sum = 0;
    for (auto const& a : allocs) {
        if (a.internal) continue;
        if (!alloc_pending(a)) sum += a.size();
    }
    return sum;
}

// Relevé complet, lu UNE fois par affichage : partagé par la page MEMOIRE et
// par sysinfo_log_memory().
struct Mem {
    size_t   int_free, int_total, int_min, int_used, int_largest;
    uint32_t dma_free, dma_min;
    bool     has_ps;
    size_t   ps_free, ps_total, ps_min, ps_largest;
    uint32_t esp_sr;
    long     untracked_int;   // interne utilisé - postes connus - ESP_SR
    long     untracked_ps;    // PSRAM utilisée  - postes connus
};

static Mem read() {
    Mem m{};
    m.int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.int_total   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    m.int_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    m.int_used    = m.int_total - m.int_free;
    m.int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    m.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    m.dma_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);

    m.has_ps = psramFound();
    if (m.has_ps) {
        m.ps_free    = ESP.getFreePsram();
        m.ps_total   = ESP.getPsramSize();
        m.ps_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        m.ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    }

    m.esp_sr = wakeword_esp_sr_internal_bytes();

    // Reste SIGNÉ : un négatif signalerait que la table sur-compte.
    m.untracked_int = (long)m.int_used - (long)known_internal() - (long)m.esp_sr;
    m.untracked_ps  = m.has_ps ? (long)(m.ps_total - m.ps_free) - (long)known_psram() : 0;
    return m;
}

static float used_frac(size_t freeB, size_t totalB) {
    return totalB ? 1.0f - (float)freeB / (float)totalB : 0.0f;
}

}  // namespace inv


// ════════════════════════════════════════════════════════════
// PAGE 1 — IDENTITE
//   FIXE   : bandeau puce, cadres des 3 cartes, grilles MATERIEL et SYSTEME
//   BLITTÉ : la valeur + la jauge des 3 cartes (CPU0 / CPU1 / TEMP) ET la
//            ligne d'uptime, juste dessous — UNE bande contiguë
//            -> 1 Hz, blit_rows(LIVE_Y, LIVE_H)
//
// ════════════════════════════════════════════════════════════

namespace page_chip {

// -- géométrie & état --
constexpr int HERO_Y   = SI_PAGE_Y0;        // nom de puce en double taille
constexpr int HERO_SUB = HERO_Y + 18;       // révision / cœurs / fréquence

constexpr int CARD_Y   = HERO_SUB + 17;     // les 3 cartes de charge
constexpr int CARD_W   = 99;
constexpr int CARD_TH  = 11;                // bandeau titré
constexpr int CARD_H   = CARD_TH + 21;
constexpr int CARD_X[] = { SI_LX, 111, 214 };

constexpr int VAL_Y    = CARD_Y + CARD_TH + 3;   // valeur en double taille
constexpr int VAL_W    = 4 * 12;                 // "100%" / " 54C", largeur figée
constexpr int GAU_X    = 4 + VAL_W + 4;          // jauge, relative à la carte
constexpr int GAU_W    = CARD_W - 4 - GAU_X;

constexpr int UP_Y     = CARD_Y + CARD_H + 4;    // uptime, dans la même bande
constexpr int LIVE_Y   = VAL_Y - 1;
constexpr int LIVE_H   = UP_Y + SI_LH - LIVE_Y;

constexpr int GX[]     = { SI_LX, 164 };    // grilles de faits, 2 colonnes
constexpr int GW       = 148;

// ---- BLITTÉ ----
// Une carte : valeur en gros, jauge à droite. pct < 0 = pas de fenêtre de mesure.
static void card_value(int i, const char* val, float frac, uint16_t c) {
    int cx = CARD_X[i];
    draw::big(cx + 4, VAL_Y, val, c);
    draw::bar(cx + GAU_X, VAL_Y + 3, GAU_W, draw::BAR_HERO, frac, c);
}

static void draw_live(int c0, int c1) {
    char buf[32];

    for (int i = 0; i < 3; i++)
        surface::fill_rect(CARD_X[i] + 1, LIVE_Y, CARD_W - 2, VAL_Y + 16 - LIVE_Y, C_BG);
    draw::wipe_rows(UP_Y, SI_LH);

    const int pct[2] = { c0, c1 };
    for (int i = 0; i < 2; i++) {
        if (pct[i] < 0) { card_value(i, "  --", 0, C_DIM); continue; }
        snprintf(buf, sizeof(buf), "%3d%%", pct[i]);
        card_value(i, buf, pct[i] / 100.0f, pct[i] > 90 ? C_ORANGE : C_GREEN);
    }

    // Jauge de température bornée à 100 C : au-delà la puce a d'autres soucis.
    int t = (int)temperatureRead();
    snprintf(buf, sizeof(buf), "%3dC", t);
    card_value(2, buf, t / 100.0f, t < 50 ? C_GREEN : t < 70 ? C_YELLOW : C_RED);

    // Uptime — esp_timer (u64) et non millis(), qui reboucle à 49,7 jours.
    uint64_t s = (uint64_t)(esp_timer_get_time() / 1000000);
    unsigned long d = (unsigned long)(s / 86400);
    if (d) snprintf(buf, sizeof(buf), "%lu j  %02lu:%02lu:%02lu", d,
                    (unsigned long)(s / 3600 % 24), (unsigned long)(s / 60 % 60),
                    (unsigned long)(s % 60));
    else   snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                    (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                    (unsigned long)(s % 60));
    draw::text(SI_LX, UP_Y, "Uptime", C_DKCYAN);
    draw::text(SI_LX + 66, UP_Y, buf, C_CYAN);
}

static void refresh() {
    int c0 = -1, c1 = -1;
    cpu::load(&c0, &c1);
    draw_live(c0, c1);
    surface::blit_rows(LIVE_Y, LIVE_H);
}

// ---- FIXE ----
// Une case de grille : intitulé à gauche, valeur alignée à droite de la colonne.
static void cell(int col, int y, const char* key, const char* val, uint16_t c) {
    draw::text(GX[col], y, key, C_DKCYAN);
    draw::text_right(GX[col] + GW, y, val, c);
}

static void draw_static() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[48];

    // -- bandeau puce --
    draw::big(SI_LX, HERO_Y, fmt::chip_model(chip.model), C_CYAN);
    snprintf(buf, sizeof(buf), "rev %d  -  %d coeurs @ %d MHz",
             chip.revision, chip.cores, getCpuFrequencyMhz());
    draw::text(SI_LX, HERO_SUB, buf, C_DIM);

    // -- cadres des 3 cartes (l'intérieur appartient à draw_live) --
    const char* titles[3] = { "CPU 0", "CPU 1", "TEMP DIE" };
    const uint16_t strips[3] = { C_DKCYAN, C_MAGENTA_DK, C_DIM };   // mêmes teintes
    for (int i = 0; i < 3; i++) {                                   // que les badges
        int cx = CARD_X[i];                                         // de la page TACHES
        surface::rect(cx, CARD_Y, CARD_W, CARD_H, strips[i]);
        surface::fill_rect(cx + 1, CARD_Y + 1, CARD_W - 2, CARD_TH - 1, strips[i]);
        draw::text(cx + 4, CARD_Y + 3, titles[i], C_WHITE, strips[i]);
    }

    int y = UP_Y + SI_LH + 4;
    draw::hline(y); y += 5;
    draw::section(y, ">> MATERIEL"); y += SI_LH + 2;

    cell(0, y, "Ecran", "ILI9341V", C_WHITE);
    snprintf(buf, sizeof(buf), "%d MB", ESP.getFlashChipSize() / (1024 * 1024));
    cell(1, y, "Flash",      buf,  C_YELLOW);
    y += SI_LH;

    snprintf(buf, sizeof(buf), "%dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);
    cell(0, y, "Resolution", buf, C_WHITE);
    snprintf(buf, sizeof(buf), "%lu MHz %s",
             (unsigned long)(ESP.getFlashChipSpeed() / 1000000),
             fmt::flash_mode(ESP.getFlashChipMode()));
    cell(1, y, "Mode flash", buf, C_WHITE);
    y += SI_LH;

    // ⚠️ La fréquence RÉELLE, pas TFT_SPI_HZ : le contrôleur arrondit à un
    // diviseur entier de l'APB, demander 60 MHz donne 40 (cf. display_driver.h).
    snprintf(buf, sizeof(buf), "%lu MHz", (unsigned long)(panel_actual_hz() / 1000000));
    cell(0, y, "Bus SPI", buf, C_YELLOW);
    if (psramFound()) snprintf(buf, sizeof(buf), "%lu MB",
                               (unsigned long)(ESP.getPsramSize() / (1024 * 1024)));
    else              snprintf(buf, sizeof(buf), "absente");
    cell(1, y, "PSRAM", buf, psramFound() ? C_YELLOW : C_RED);
    y += SI_LH + 4;

    draw::hline(y); y += 5;
    draw::section(y, ">> SYSTEME"); y += SI_LH + 2;

    draw::pair(SI_LX, y, "Firmware    : ", __DATE__ " " __TIME__, C_WHITE); y += SI_LH;
    draw::pair(SI_LX, y, "IDF/Arduino : ", ESP.getSdkVersion(),   C_YELLOW); y += SI_LH;

    esp_reset_reason_t rr = esp_reset_reason();
    bool bad = (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
                rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
    uint16_t rrColor = bad ? C_RED : (rr == ESP_RST_POWERON || rr == ESP_RST_SW) ? C_GREEN : C_YELLOW;
    draw::pair(SI_LX, y, "Dernier rst : ", fmt::reset_reason(rr), rrColor);
}

// ---- ASSEMBLAGE ----
static void draw() {
    draw_static();
    // Charge instantanée, mesurée entre cet affichage et le précédent
    // (cf. cpu::sample). "--" tant qu'aucune fenêtre exploitable.
    int c0 = -1, c1 = -1;
    cpu::load(&c0, &c1);
    draw_live(c0, c1);
}

}  // namespace page_chip


// ════════════════════════════════════════════════════════════
// PAGE 2 — MEMOIRE
//   FIXE   : les deux cadres de carte + leur bandeau, la liste des postes
//   BLITTÉ : l'intérieur des deux cartes — jauge empilée + 4 valeurs
//            -> UNE bande pleine largeur, un tick sur EVERY_N
//
// Deux cartes jumelles, une par domaine : RAM interne à gauche, PSRAM à droite.
// La jauge empile connu / non tracé / libre ; en dessous, les postes de chaque
// domaine dans SA colonne, valeur alignée à droite et filet de proportion.
// ⚠️ Page la plus chère : le coût est la RELECTURE de la zone invalidée depuis
// la PSRAM, jamais ce qui est dessiné dedans. Seuls leviers : CADENCE et
// HAUTEUR blittée.
// ════════════════════════════════════════════════════════════

namespace page_mem {

// -- géométrie & état --
constexpr int CARD_W  = 152;
constexpr int CARD_H  = 80;
constexpr int CARD_Y  = SI_PAGE_Y0 + SI_LH + 2;
constexpr int CARD_LX = SI_LX - 2;
constexpr int CARD_RX = SI_W - SI_LX + 2 - CARD_W;
constexpr int PAD     = 5;

constexpr int TITLE_H = 13;                       // bandeau de carte
constexpr int BIG_Y   = TITLE_H + 3;              // tout ce qui suit est
constexpr int GAUGE_Y = BIG_Y + 18;               // relatif au haut de la carte
constexpr int GAUGE_H = draw::BAR_HERO;
constexpr int VAL_Y   = GAUGE_Y + GAUGE_H + 3;
constexpr int VAL_LH  = 10;
constexpr int VAL_N   = 3;

constexpr int LIVE_Y  = CARD_Y + BIG_Y - 1;                   // bande BLITTÉE
constexpr int LIVE_H  = (VAL_Y + VAL_N * VAL_LH) - BIG_Y + 1;

// ⚠️ 18 postes à loger sous les cartes : ni en-tête de section ni filet de
// séparation, et interligne à 11 px -> 10 lignes par colonne. 11 est le
// PLANCHER : le filet de proportion occupe y+9 et y+10, à 10 px il passerait
// sous le texte de la ligne suivante.
constexpr int LIST_Y  = CARD_Y + CARD_H + 4;      // 1re ligne de la liste
constexpr int LIST_LH = 11;
constexpr int COL_W   = CARD_W - PAD * 2;         // largeur utile d'une colonne

constexpr int EVERY_N = 2;      // un rafraîchissement sur deux : le heap ne
                                // bouge pas assez vite pour justifier 1 Hz

// ---- BLITTÉ ----
// Jauge empilée : connu (teinte sourde) | non tracé (teinte vive) | libre (fond).
static void gauge(int x, int y, uint32_t known, long untracked, uint32_t total,
                  uint16_t c, uint16_t cdk) {
    const int iw = COL_W - 2;
    surface::rect(x, y, COL_W, GAUGE_H, C_DIM);
    if (!total) return;

    uint32_t u  = untracked > 0 ? (uint32_t)untracked : 0;
    int      wk = (int)((uint64_t)known * iw / total);
    int      wu = (int)((uint64_t)u * iw / total);
    if (wk > iw)      wk = iw;
    if (wk + wu > iw) wu = iw - wk;

    surface::fill_rect(x + 1,           y + 1, wk,           GAUGE_H - 2, cdk);
    surface::fill_rect(x + 1 + wk,      y + 1, wu,           GAUGE_H - 2, c);
    surface::fill_rect(x + 1 + wk + wu, y + 1, iw - wk - wu, GAUGE_H - 2, C_BG);
}

// Une ligne de valeur d'une carte : intitulé à gauche, valeur alignée à droite.
static void val(int cx, int i, const char* key, const char* v, uint16_t c) {
    int y = CARD_Y + VAL_Y + i * VAL_LH;
    draw::text(cx + PAD, y, key, C_DKCYAN);
    draw::text_right(cx + CARD_W - PAD, y, v, c);
}

// LA valeur de la carte, en double taille : ce qui reste.
static void hero(int cx, const char* v, uint16_t c) {
    int y = CARD_Y + BIG_Y;
    draw::big(cx + PAD, y, v, c);
    draw::text(cx + PAD + gfx::text_w(v, 2) + 5, y + 8, "libre", C_DIM);
}

static void draw_live(const inv::Mem& m) {
    char buf[24];

    surface::fill_rect(CARD_LX + 1, LIVE_Y, CARD_W - 2, LIVE_H, C_BG);
    surface::fill_rect(CARD_RX + 1, LIVE_Y, CARD_W - 2, LIVE_H, C_BG);

    // -- carte gauche : RAM interne --
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.int_free / 1024));
    hero(CARD_LX, buf, C_GREEN);

    gauge(CARD_LX + PAD, CARD_Y + GAUGE_Y,
          inv::known_internal() + m.esp_sr, m.untracked_int, m.int_total,
          C_MEM_INT, C_MEM_INT_DK);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.int_min / 1024));
    val(CARD_LX, 0, "Min. atteint", buf, C_YELLOW);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.dma_free / 1024));
    val(CARD_LX, 1, "DMA libre", buf, C_WHITE);

    fmt::size(buf, sizeof(buf), m.untracked_int);
    val(CARD_LX, 2, "Non trace", buf, C_MEM_INT);

    // -- carte droite : PSRAM --
    if (!m.has_ps) {
        draw::text(CARD_RX + PAD, CARD_Y + BIG_Y + 4, "NON DETECTEE", C_RED);
        return;
    }

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.ps_free / 1024));
    hero(CARD_RX, buf, C_GREEN);

    gauge(CARD_RX + PAD, CARD_Y + GAUGE_Y,
          inv::known_psram(), m.untracked_ps, m.ps_total,
          C_MEM_EXT, C_MEM_EXT_DK);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.ps_min / 1024));
    val(CARD_RX, 0, "Min. atteint", buf, C_YELLOW);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(m.ps_largest / 1024));
    val(CARD_RX, 1, "Bloc max",  buf, C_WHITE);

    fmt::size(buf, sizeof(buf), m.untracked_ps);
    val(CARD_RX, 2, "Non trace", buf, C_MEM_EXT);
}

static void refresh() {
    inv::Mem m = inv::read();
    draw_live(m);
    surface::blit_rows(LIVE_Y, LIVE_H);
}

// ---- FIXE ----
// Cadre + bandeau titré. L'intérieur reste vide : il appartient à draw_live().
static void card_frame(int cx, const char* title, uint32_t total,
                       uint16_t c, uint16_t cdk) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(total / 1024));

    surface::rect(cx, CARD_Y, CARD_W, CARD_H, cdk);
    surface::fill_rect(cx + 1, CARD_Y + 1, CARD_W - 2, TITLE_H - 1, cdk);
    draw::text(cx + PAD, CARD_Y + 4, title, C_WHITE, cdk);
    draw::text_right(cx + CARD_W - PAD, CARD_Y + 4, buf, c, cdk);
}

// Postes d'un domaine, un par ligne. ⚠️ Filet normalisé sur le PLUS GROS poste
// de la colonne, pas sur le total : sinon tout est écrasé à quelques pixels.
struct Item { const char* label; uint32_t bytes; };

static void list_col(int cx, const Item* it, int n, uint16_t c, uint16_t cdk) {
    uint32_t max = 0;
    for (int i = 0; i < n; i++) if (it[i].bytes > max) max = it[i].bytes;

    for (int i = 0; i < n; i++) {
        int  y = LIST_Y + i * LIST_LH;
        char buf[16];
        fmt::size(buf, sizeof(buf), (long)it[i].bytes);

        draw::text(cx + PAD, y, it[i].label, C_DKCYAN);
        draw::text_right(cx + CARD_W - PAD, y, buf, c);

        // Plancher à 1 px : un poste présent ne doit jamais avoir un filet vide.
        int fw = max ? (int)((uint64_t)it[i].bytes * COL_W / max) : 0;
        if (fw == 0 && it[i].bytes) fw = 1;
        surface::fill_rect(cx + PAD, y + 9, COL_W, draw::RULE, C_GRID);
        if (fw > 0) surface::fill_rect(cx + PAD, y + 9, fw, draw::RULE, cdk);
    }
}

static void draw_static(const inv::Mem& m) {
    draw::section(SI_PAGE_Y0, ">> BILAN MEMOIRE");

    card_frame(CARD_LX, "RAM INTERNE", m.int_total, C_MEM_INT, C_MEM_INT_DK);
    card_frame(CARD_RX, "PSRAM",       m.ps_total,  C_MEM_EXT, C_MEM_EXT_DK);

    // ESP_SR et les piles ne sont pas dans inv::allocs (mesure au boot, et
    // miroir de config.h) : ils rejoignent ici la colonne interne, d'où le
    // dimensionnement. ⚠️ Sur les tables, pas à la main — un poste de plus
    // déborderait sans rien dire.
    Item li[inv::alloc_count + 1 + inv::stack_count], ri[inv::alloc_count];
    int  nl = 0, nr = 0;
    for (auto const& a : inv::allocs) {
        if (a.internal) li[nl++] = { a.label, a.size() };
        else            ri[nr++] = { a.label, a.size() };
    }
    li[nl++] = { "ESP_SR", m.esp_sr };
    for (auto const& s : inv::stacks) li[nl++] = { s.name, s.stack };

    list_col(CARD_LX, li, nl, C_MEM_INT, C_MEM_INT_DK);
    list_col(CARD_RX, ri, nr, C_MEM_EXT, C_MEM_EXT_DK);
}

// ---- ASSEMBLAGE ----
static void draw() {
    inv::Mem m = inv::read();
    draw_static(m);
    draw_live(m);
}

}  // namespace page_mem




// ════════════════════════════════════════════════════════════
// PAGE 3 — TACHES
//   FIXE   : titres de colonnes, NOM + badge cœur de chaque ligne
//   BLITTÉ : le bandeau de charge (1 rangée pleine largeur) ET les colonnes
//            ETAT / PRI / %CPU / PILE avec leurs jauges (x >= LIVE_X)
//            -> 1 Hz, deux blits DISJOINTS
//
// Etats : X=actif  R=pret  B=bloque  S=suspendu  D=supprime — la LETTRE dit
// quoi, la COULEUR dit à quel point ça compte.
//
// Tâches IDLE EXCLUES : leur temps sert déjà à la charge par cœur du bandeau.
// Tri par ÉTAT puis %CPU décroissant, pour que ce qui déborde de la page soit
// ce qui dort. ⚠️ Ordre FIGÉ à l'entrée : le rejouer ferait sauter les lignes.
// ════════════════════════════════════════════════════════════

namespace page_tasks {

// -- géométrie & état --
constexpr int COL_NAME  = SI_LX;        // 13 caractères
constexpr int COL_CORE  = 88;           // badge cœur
constexpr int LIVE_X    = 100;          // tout ce qui suit est BLITTÉ
constexpr int COL_STATE = LIVE_X;
constexpr int COL_PRI   = 110;
constexpr int COL_CPU   = 128;
constexpr int CPU_BAR_X = 156;
constexpr int CPU_BAR_W = 52;
constexpr int COL_STACK = 212;
constexpr int STK_BAR_X = 270;
constexpr int STK_BAR_W = SI_W - SI_LX - STK_BAR_X;

// Bandeau vedette, BLITTÉ lui aussi : figé, il mentirait pendant que les
// lignes se rafraîchissent.
constexpr int HDR_Y      = SI_PAGE_Y0;
constexpr int HDR_H      = 18;          // une seule rangée : la valeur-vedette
constexpr int HERO_BIG_X = 36;          // et son intitulé tiennent sur la même
constexpr int HERO_BAR_X = 92;          // ligne que le contexte, sinon c'est
constexpr int HERO_BAR_W = 48;          // une ligne de tâche en moins

static struct {
    TaskHandle_t order[SI_MAX_TASKS];   // ordre figé, retrouvé par handle
    UBaseType_t  shown;
    int          y0;
} L;

static uint16_t state_color(eTaskState s) {
    switch (s) {
        case eRunning:   return C_GREEN;
        case eReady:     return C_YELLOW;
        case eBlocked:   return C_DKCYAN;
        case eSuspended: return C_DIM;
        default:         return C_RED;
    }
}

// ---- BLITTÉ ----
// Bandeau : une jauge par cœur, puis nombre de tâches et heap libre.
static void draw_header(int c0, int c1, unsigned tasks) {
    char buf[48];
    draw::wipe_rows(HDR_Y, HDR_H);

    // ⚠️ Vedette sur le cœur 0 seul : le cœur 1 lit ~100 % en permanence par
    // artefact (loopTask prio 1 affame IDLE1), l'afficher en gros serait une
    // alarme permanente.
    draw::text(SI_LX, HDR_Y + 4, "CPU0", C_DKCYAN);

    if (c0 >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", c0);
        uint16_t c = c0 > 90 ? C_ORANGE : c0 > 30 ? C_YELLOW : C_GREEN;
        draw::big(HERO_BIG_X, HDR_Y, buf, c);
        draw::bar(HERO_BAR_X, HDR_Y + 2, HERO_BAR_W, draw::BAR_HERO, c0 / 100.0f, c);
    } else {
        draw::big(HERO_BIG_X, HDR_Y, "--", C_DIM);
        draw::bar(HERO_BAR_X, HDR_Y + 2, HERO_BAR_W, draw::BAR_HERO, 0, C_DIM);
    }

    if (c1 >= 0) snprintf(buf, sizeof(buf), "CPU1 %d%%   %u taches", c1, tasks);
    else         snprintf(buf, sizeof(buf), "CPU1 --   %u taches", tasks);
    draw::text_right(SI_W - SI_LX, HDR_Y + 4, buf, C_DIM);
}

// Colonnes vivantes d'une ligne (le nom et le cœur, eux, ne bougent pas).
static void draw_live_row(int rowY, const TaskStatus_t* t, uint8_t pct, bool cpu_ok) {
    char buf[32];
    surface::fill_rect(LIVE_X, rowY, SI_W - LIVE_X, SI_LH, C_BG);

    if (!t) {   // tâche disparue depuis l'entrée sur la page
        draw::text(LIVE_X, rowY, "(terminee)", C_DIM);
        return;
    }

    char st[2] = { fmt::task_state(t->eCurrentState), '\0' };
    draw::text(COL_STATE, rowY, st, state_color(t->eCurrentState));

    snprintf(buf, sizeof(buf), "%2u", (unsigned)t->uxCurrentPriority);
    draw::text(COL_PRI, rowY, buf, C_DKCYAN);

    // %CPU — orange au-delà de 30 %, rouge au-delà de 70 %.
    if (cpu_ok) {
        uint16_t c = pct > 70 ? C_RED : pct > 30 ? C_ORANGE : C_GREEN;
        snprintf(buf, sizeof(buf), "%3u%%", (unsigned)pct);
        draw::text(COL_CPU, rowY, buf, pct > 30 ? c : C_WHITE);
        draw::bar(CPU_BAR_X, rowY + 1, CPU_BAR_W, draw::BAR_ROW, pct / 100.0f, c);
    } else {
        draw::text(COL_CPU, rowY, "  --", C_DIM);
    }

    // Pile : high-water (octets LIBRES). Rouge < 512 o, orange < 1 Ko. Jauge
    // seulement pour NOS tâches : le total alloué n'est connu que d'elles.
    unsigned long freeBytes = (unsigned long)t->usStackHighWaterMark * sizeof(StackType_t);
    uint16_t stackTotal = inv::stack_size(t->pcTaskName);
    uint16_t sc = (freeBytes < 512) ? C_RED : (freeBytes < 1024) ? C_ORANGE : C_GREEN;

    if (stackTotal) {
        snprintf(buf, sizeof(buf), "%lu/%u", freeBytes, (unsigned)stackTotal);
        draw::text(COL_STACK, rowY, buf, sc);
        draw::bar(STK_BAR_X, rowY + 1, STK_BAR_W, draw::BAR_ROW,
                  1.0f - (float)freeBytes / (float)stackTotal, sc);
    } else {
        snprintf(buf, sizeof(buf), "%lu", freeBytes);
        draw::text(COL_STACK, rowY, buf, sc);
    }
}

static void refresh() {
    if (L.shown == 0) return;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);
    if (n == 0) return;

    uint8_t pct[SI_MAX_TASKS];
    int  c0 = -1, c1 = -1;
    bool cpu_ok = cpu::sample(st, n, pct, &c0, &c1);

    draw_header(c0, c1, (unsigned)n);
    surface::blit_rows(HDR_Y, HDR_H);

    for (UBaseType_t i = 0; i < L.shown; i++) {
        const TaskStatus_t* found = nullptr;
        uint8_t p = 0;
        for (UBaseType_t j = 0; j < n; j++) {
            if (st[j].xHandle == L.order[i]) { found = &st[j]; p = pct[j]; break; }
        }
        draw_live_row(L.y0 + (int)(i * SI_LH), found, p, cpu_ok);
    }

    surface::blit_rect(LIVE_X, L.y0, SI_W - LIVE_X, (int)(L.shown * SI_LH));
}

// ---- ASSEMBLAGE ----
// ⚠️ Le FIXE dépend du relevé (noms triés par %CPU) : draw_static et draw_live
// sont indissociables au premier rendu, d'où un draw() d'un seul tenant.
static void draw() {
    char buf[64];
    int y = SI_PAGE_Y0;

    L.shown = 0;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);

    if (n == 0) {
        draw::text(SI_LX, y, "(uxTaskGetSystemState: buffer trop petit)", C_RED);
        return;
    }

    uint8_t pct[SI_MAX_TASKS];
    int c0 = -1, c1 = -1;
    bool cpu_ok = cpu::sample(st, n, pct, &c0, &c1);

    draw_header(c0, c1, (unsigned)n);
    y += HDR_H;

    draw::hline(y); y += 5;

    // Écarte les IDLE, en compactant st[]/pct[] au passage
    UBaseType_t m = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strncmp(st[i].pcTaskName, "IDLE", 4) == 0) continue;
        st[m]  = st[i];
        pct[m] = pct[i];
        m++;
    }

    // Tri : état croissant (running d'abord), puis %CPU décroissant.
    for (UBaseType_t i = 0; i < m; i++) {
        for (UBaseType_t j = i + 1; j < m; j++) {
            uint8_t ri = fmt::task_rank(st[i].eCurrentState);
            uint8_t rj = fmt::task_rank(st[j].eCurrentState);
            bool swap = (rj < ri) || (rj == ri && pct[j] > pct[i]);
            if (swap) {
                TaskStatus_t t = st[i]; st[i] = st[j]; st[j] = t;
                uint8_t p = pct[i]; pct[i] = pct[j]; pct[j] = p;
            }
        }
    }

    // Titres de colonnes
    draw::text(COL_NAME,  y, "NOM",          C_DKCYAN);
    draw::text(COL_CORE,  y, "C",            C_DKCYAN);
    draw::text(COL_STATE, y, "E",            C_DKCYAN);
    draw::text(COL_PRI,   y, "PRI",          C_DKCYAN);
    draw::text(COL_CPU,   y, "%CPU",         C_DKCYAN);
    draw::text(COL_STACK, y, "PILE LIB/TOT", C_DKCYAN);
    y += SI_LH;

    UBaseType_t maxRows = (UBaseType_t)((SI_PAGE_YMAX - y) / SI_LH);
    // ⚠️ Le compteur de reste doit rester dans la zone effacée par
    // frame::clear_content, sinon il s'imprime sur le pied de page.
    UBaseType_t shown = (m <= maxRows) ? m : (maxRows > 0 ? maxRows - 1 : 0);

    L.y0    = y;
    L.shown = shown;

    for (UBaseType_t i = 0; i < shown; i++) {
        int rowY = y + (int)(i * SI_LH);
        L.order[i] = st[i].xHandle;

        // -- FIXE de la ligne : nom + badge cœur --
        snprintf(buf, sizeof(buf), "%.13s", st[i].pcTaskName ? st[i].pcTaskName : "?");
        draw::text(COL_NAME, rowY, buf, C_WHITE);

        // '*' = sans affinité (ordonnancée sur l'un ou l'autre).
        int  core = (int)st[i].xCoreID;
        bool pinned = (core == 0 || core == 1);
        char cbuf[2] = { pinned ? (char)('0' + core) : '*', '\0' };
        uint16_t badge = !pinned ? C_DIM : (core == 0) ? C_DKCYAN : C_MAGENTA_DK;
        surface::fill_rect(COL_CORE - 1, rowY - 1, 8, 10, badge);
        draw::text(COL_CORE, rowY, cbuf, C_WHITE, badge);

        // -- BLITTÉ de la ligne --
        draw_live_row(rowY, &st[i], pct[i], cpu_ok);
    }

    if (m > shown) {
        snprintf(buf, sizeof(buf), "+ %u autre(s) non affichee(s)", (unsigned)(m - shown));
        draw::text(SI_LX, y + (int)(shown * SI_LH), buf, C_DIM);
    }
}

}  // namespace page_tasks


// ════════════════════════════════════════════════════════════
// PAGE 4 — PARTITIONS FLASH
//   FIXE   : tout
//   BLITTÉ : néant
//
// Pastilles ACTIF/BOOT, table des partitions avec jauge d'occupation par ligne,
// puis un graphe en barre empilée représentant l'occupation de la flash entière.
// ════════════════════════════════════════════════════════════

namespace page_part {

// -- géométrie --
constexpr int COL_LABEL = SI_LX;
constexpr int COL_TYPE  = 72;
constexpr int COL_SIZE  = 168;   // aligné à DROITE
constexpr int COL_USED  = 204;   // aligné à DROITE
constexpr int BAR_X     = 210;
constexpr int BAR_W     = 42;
constexpr int COL_OFF   = 258;
constexpr int HERO_Y    = SI_PAGE_Y0 + 14;   // capacité en double taille
constexpr int GRAPH_Y    = SI_PAGE_Y0 + 36;  // la carte de la flash, EN TÊTE
constexpr int TABLE_Y    = SI_PAGE_Y0 + 85;  // 1re ligne de la table
constexpr int GRAPH_H   = 16;
constexpr int LEGEND_H  = 18;
constexpr int MAX_GRAPH_PARTS = 16;

// ⚠️ Relevé fait EN UNE FOIS avant tout dessin : le graphe, en tête de page, a
// besoin de la liste complète alors que la table n'est pas encore tracée.
struct Entry {
    uint32_t address;
    uint32_t size;
    uint16_t color;
    bool     isRunning;
    float    usedFrac;   // 0..1 = occupé ; -1 = inconnu. Sert au GRAPHE, qui ne
                         // surimprime que les partitions marquées overlay.
    bool     overlay;    // spiffs/fat seulement : la teinte C_MAGENTA_DK de la
                         // surimpression est celle de la légende, l'étendre aux
                         // app dirait "spiffs" sur un segment applicatif.
    char     label[18];
    uint8_t  type, subtype;
};

static uint16_t color_of(uint8_t type, uint8_t subtype, bool isRunning) {
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

// Taille réelle du binaire d'une partition applicative, en suivant l'en-tête
// d'image ESP : 24 o d'en-tête, N segments (8 o + data_len), un checksum aligné
// sur 16 o, puis le SHA256 s'il est annexé.
// -1 si pas d'image valide — cas normal d'un slot OTA jamais écrit.
static long image_size(const esp_partition_t* p) {
    esp_image_header_t hdr;
    if (esp_partition_read(p, 0, &hdr, sizeof(hdr)) != ESP_OK) return -1;
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) return -1;
    if (hdr.segment_count == 0 || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS) return -1;

    size_t off = sizeof(esp_image_header_t);
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (off + sizeof(seg) > p->size) return -1;
        if (esp_partition_read(p, off, &seg, sizeof(seg)) != ESP_OK) return -1;
        // Flash vierge lue comme des segments : sortir par -1, jamais boucler.
        if (seg.data_len > p->size) return -1;
        off += sizeof(seg) + seg.data_len;
        if (off > p->size) return -1;
    }

    off = (off + 1 + 15) & ~(size_t)15;   // octet de checksum, aligné sur 16 o
    if (hdr.hash_appended) off += 32;     // SHA256 "simple hash", inclus dans l'image

    return (off <= p->size) ? (long)off : -1;
}

// Octets réellement occupés dans une partition. -1 quand la question n'a pas de
// réponse lisible — affiché "--" plutôt qu'un 0 trompeur.
//   app en cours : ESP.getSketchSize(), l'API éprouvée du core
//   app inactive : image_size() ci-dessus
//   littlefs     : littlefs_used_bytes()
//   nvs / otadata / coredump / model : format interne, pas de notion
//                  d'occupation lisible au runtime
//
// ⚠️ Reconnaissance par LABEL, pas par subtype : `partitions.csv` déclare DEUX
// partitions de subtype `spiffs` (`littlefs` et `model`), et filtrer sur le
// subtype attribue l'occupation de LittleFS aux deux.
static long used_bytes(const esp_partition_t* p, bool isRunning) {
    if (p->type == ESP_PARTITION_TYPE_APP)
        return isRunning ? (long)ESP.getSketchSize() : image_size(p);

    // Le label est celui passé à LittleFS.begin() dans littlefs_manager.cpp.
    if (strcmp(p->label, "littlefs") == 0 && littlefs_is_mounted())
        return (long)littlefs_used_bytes();

    return -1;
}

// Barre empilée = flash entière, à l'échelle des adresses réelles.
static void draw_graph(const Entry* e, int count, uint32_t flashTotal, int gy) {
    const int gx = SI_LX;
    const int gw = SI_W - SI_LX * 2;

    surface::rect(gx, gy, gw, GRAPH_H, C_DIM);

    uint32_t cursorAddr = 0;
    int      cursorX    = gx + 1;
    int      innerW     = gw - 2;

    for (int i = 0; i < count; i++) {
        // Espace non alloué avant cette partition (bootloader, table de part.)
        if (e[i].address > cursorAddr) {
            uint32_t gap = e[i].address - cursorAddr;
            int gapW = (int)((uint64_t)gap * innerW / flashTotal);
            if (gapW > 0) {
                surface::fill_rect(cursorX, gy + 1, gapW, GRAPH_H - 2, C_GRID);
                cursorX += gapW;
            }
        }

        int segW = (int)((uint64_t)e[i].size * innerW / flashTotal);
        if (segW < 1) segW = 1;
        surface::fill_rect(cursorX, gy + 1, segW, GRAPH_H - 2, e[i].color);

        // Surimpression : portion réellement occupée (spiffs/fat, réservé large)
        if (e[i].overlay && e[i].usedFrac >= 0.0f) {
            int usedW = (int)(segW * constrain(e[i].usedFrac, 0.0f, 1.0f));
            if (usedW > 0) surface::fill_rect(cursorX, gy + 1, usedW, GRAPH_H - 2, C_MAGENTA_DK);
        }

        // Délimiteur sur CHAQUE segment : sans ça, deux partitions de même
        // couleur se lisent comme un seul bloc.
        surface::rect(cursorX, gy + 1, segW, GRAPH_H - 2,
                              e[i].isRunning ? C_WHITE : C_DKCYAN);

        cursorX += segW;
        cursorAddr = e[i].address + e[i].size;
    }

    if (cursorX < gx + 1 + innerW)
        surface::fill_rect(cursorX, gy + 1, gx + 1 + innerW - cursorX, GRAPH_H - 2, C_GRID);
}

static void draw_legend(int ly) {
    const int swatch = 8;
    int lx = SI_LX;

    auto item = [&](uint16_t color, const char* label) {
        if (lx + swatch + (int)strlen(label) * 6 + 10 > SI_W - SI_LX) {
            lx = SI_LX;
            ly += 9;
        }
        surface::fill_rect(lx, ly, swatch, swatch, color);
        surface::rect(lx, ly, swatch, swatch, C_DIM);
        draw::text(lx + swatch + 2, ly, label, C_DIM);
        lx += swatch + (int)strlen(label) * 6 + 10;
    };

    item(C_YELLOW,     "nvs");
    item(C_DKCYAN,     "app inactif");
    item(C_GREEN,      "app actif");
    item(C_MAGENTA,    "spiffs/fat");
    // Entrée de légende de la surimpression sombre.
    item(C_MAGENTA_DK, "littlefs occupe");
    item(C_GRID,       "libre/system");
}

static void draw() {
    char buf[64];

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();
    uint32_t flashTotal = ESP.getFlashChipSize();

    // -- relevé complet, avant tout dessin --
    Entry entries[MAX_GRAPH_PARTS];
    int   count = 0;

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr && count < MAX_GRAPH_PARTS) {
        const esp_partition_t* p = esp_partition_get(it);
        bool isRunning = running && (p->address == running->address);

        // Occupation réelle — lue UNE fois, partagée par la colonne UTIL% et par
        // la surimpression du graphe.
        long  usedB    = used_bytes(p, isRunning);
        Entry& e = entries[count++];
        e.address   = p->address;
        e.size      = p->size;
        e.color     = color_of(p->type, p->subtype, isRunning);
        e.isRunning = isRunning;
        e.usedFrac  = (usedB >= 0 && p->size) ? (float)usedB / (float)p->size : -1.0f;
        e.overlay   = (strcmp(p->label, "littlefs") == 0);
        e.type      = p->type;
        e.subtype   = p->subtype;
        snprintf(e.label, sizeof(e.label), "%s%s", p->label, isRunning ? "*" : "");

        it = esp_partition_next(it);   // libère l'itérateur au dernier appel
    }

    // -- bandeau vedette : la capacité, puis la carte de la flash --
    int y = SI_PAGE_Y0;
    draw::section(y, ">> MEMOIRE FLASH");

    snprintf(buf, sizeof(buf), "%lu MB", (unsigned long)(flashTotal / (1024 * 1024)));
    draw::big(SI_LX, HERO_Y, buf, C_YELLOW);

    // Pastilles : ce qui tourne, et ce qui tournera au prochain reboot.
    int bx = SI_LX + gfx::text_w(buf, 2) + 12;
    if (running) {
        snprintf(buf, sizeof(buf), "ACTIF  %s  @ 0x%06X", running->label, (unsigned)running->address);
        bx += draw::badge(bx, HERO_Y + 4, buf, C_GREEN) + 8;
    } else {
        bx += draw::badge(bx, HERO_Y + 4, "ACTIF  INCONNU", C_RED, C_WHITE) + 8;
    }
    if (boot && boot != running) {
        snprintf(buf, sizeof(buf), "BOOT  %s", boot->label);
        draw::badge(bx, HERO_Y + 4, buf, C_YELLOW);
    }

    draw_graph(entries, count, flashTotal, GRAPH_Y);
    draw_legend(GRAPH_Y + GRAPH_H + 4);

    // -- table des partitions --
    y = TABLE_Y;
    draw::hline(y - 5);

    draw::text(COL_LABEL, y, "LABEL",  C_DKCYAN);
    draw::text(COL_TYPE,  y, "TYPE",   C_DKCYAN);
    draw::text_right(COL_SIZE, y, "TAILLE", C_DKCYAN);
    draw::text_right(COL_USED, y, "UTIL%",  C_DKCYAN);
    draw::text(COL_OFF,   y, "OFFSET", C_DKCYAN);
    y += SI_LH;

    int maxRows = (SI_PAGE_YMAX - y) / SI_LH;

    for (int i = 0; i < count && i < maxRows; i++) {
        const Entry& e = entries[i];

        draw::text(COL_LABEL, y, e.label, e.isRunning ? C_GREEN : C_WHITE);
        draw::text(COL_TYPE,  y, fmt::part_subtype(e.type, e.subtype), C_CYAN);

        snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(e.size / 1024));
        draw::text_right(COL_SIZE, y, buf, C_YELLOW);

        // Rampe de criticité : ici l'orange/rouge dit bien "ça se remplit".
        if (e.usedFrac >= 0.0f) {
            unsigned pctUsed = (unsigned)(e.usedFrac * 100.0f + 0.5f);
            uint16_t c = pctUsed >= 90 ? C_RED : pctUsed >= 70 ? C_ORANGE : C_GREEN;
            snprintf(buf, sizeof(buf), "%u%%", pctUsed);
            draw::text_right(COL_USED, y, buf, c);
            draw::bar(BAR_X, y + 1, BAR_W, draw::BAR_ROW, e.usedFrac, c);
        } else {
            // nvs/otadata/model : format interne, pas d'occupation lisible.
            draw::text_right(COL_USED, y, "--", C_DIM);
            surface::rect(BAR_X, y + 1, BAR_W, draw::BAR_ROW, C_GRID);
        }

        snprintf(buf, sizeof(buf), "0x%06X", (unsigned)e.address);
        draw::text(COL_OFF, y, buf, C_DIM);
        y += SI_LH;
    }
}

}  // namespace page_part


// ════════════════════════════════════════════════════════════
// PAGE 5 — LITTLEFS
//   FIXE   : tout
//   BLITTÉ : néant
//
// Jauge d'occupation en tête, puis une ligne par entrée de la racine avec un
// filet donnant sa part de l'occupation. Sous-répertoires agrégés en une ligne
// "[DIR] nom (N)".
// ⚠️ openNextFile() est TOUJOURS rappelé sur le handle du répertoire, jamais sur
// l'entrée renvoyée, et un sous-répertoire doit être rouvert par chemin pour
// obtenir son propre curseur.
// ════════════════════════════════════════════════════════════

namespace page_fs {

// -- géométrie --
constexpr int COL_NAME = SI_LX;
constexpr int COL_SIZE = SI_W - SI_LX;          // aligné à DROITE
constexpr int HERO_Y   = SI_PAGE_Y0 + SI_LH + 2;
constexpr int GAU_X    = 70;
constexpr int GAU_W    = SI_W - SI_LX - GAU_X;

// Une entrée de la racine : nom, taille à droite, filet de part d'occupation.
static void entry_row(int y, const char* name, uint16_t nameColor,
                      uint16_t barColor, size_t bytes, size_t used) {
    char buf[16];
    fmt::size(buf, sizeof(buf), (long)bytes);

    draw::text(COL_NAME, y, name, nameColor);
    draw::text_right(COL_SIZE, y, buf, C_GREEN);

    // Part de l'OCCUPATION, pas de la capacité : rapporté à la partition
    // entière, chaque fichier ferait un filet invisible.
    int w  = COL_SIZE - COL_NAME;
    int fw = used ? (int)((uint64_t)bytes * w / used) : 0;
    if (fw == 0 && bytes) fw = 1;
    surface::fill_rect(COL_NAME, y + 9, w, draw::RULE, C_GRID);
    if (fw > 0) surface::fill_rect(COL_NAME, y + 9, fw, draw::RULE, barColor);
}

static void draw() {
    char buf[48];
    int y = SI_PAGE_Y0;

    draw::section(y, ">> LITTLEFS");

    if (!littlefs_is_mounted()) {
        draw::badge(SI_LX, HERO_Y, "SYSTEME DE FICHIERS NON MONTE", C_RED, C_WHITE);
        return;
    }

    size_t   total = littlefs_total_bytes();
    size_t   used  = littlefs_used_bytes();
    unsigned pct   = total ? (unsigned)((uint64_t)used * 100 / total) : 0;

    // -- jauge d'occupation, en gros --
    uint16_t c = pct >= 90 ? C_RED : pct >= 70 ? C_ORANGE : C_GREEN;
    snprintf(buf, sizeof(buf), "%3u%%", pct);
    draw::big(SI_LX, HERO_Y, buf, c);
    draw::bar(GAU_X, HERO_Y + 3, GAU_W, draw::BAR_HERO, pct / 100.0f, c);

    snprintf(buf, sizeof(buf), "%lu KB occupes  /  %lu KB  -  libre %lu KB",
             (unsigned long)(used / 1024), (unsigned long)(total / 1024),
             (unsigned long)((total - used) / 1024));
    draw::text(GAU_X, HERO_Y + 20, buf, C_DIM);

    y = HERO_Y + 32;
    draw::hline(y); y += 5;

    draw::text(COL_NAME, y, "FICHIER", C_DKCYAN);
    draw::text_right(COL_SIZE, y, "TAILLE", C_DKCYAN);
    y += SI_LH;

    fs::File root = littlefs_open("/", "r");
    if (!root || !root.isDirectory()) {
        draw::badge(SI_LX, y, "RACINE INVALIDE", C_RED, C_WHITE);
        return;
    }

    // Réserve : la ligne "... suite ...", le séparateur et la ligne de résumé.
    const int maxY = SI_PAGE_YMAX - (SI_LH * 2 + 11);

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
            entry_row(y, name, C_MEM_EXT, C_MEM_EXT_DK, subSize, used);   // bleu : repertoire
            fileCount += subCount;
        } else {
            snprintf(name, sizeof(name), "%.24s", file.name());
            entry_row(y, name, C_WHITE, C_DKCYAN, file.size(), used);
            fileCount++;
        }

        y += SI_LH;

        fs::File next = root.openNextFile();
        file.close();
        file = next;
    }

    if (file) {
        draw::text(SI_LX, y, "... suite ...", C_ORANGE);
        y += SI_LH;
        file.close();
    }
    root.close();

    draw::hline(y); y += 4;

    snprintf(buf, sizeof(buf), "%u fichier(s)", fileCount);
    draw::text(SI_LX, y, buf, C_CYAN);
}

}  // namespace page_fs


// ════════════════════════════════════════════════════════════
// PAGE 6 — RESEAU
//   FIXE   : tout
//   BLITTÉ : néant
//
// Pastilles d'état, carte SIGNAL (RSSI en gros + jauge + qualité), grille
// d'adressage en deux colonnes, puis le bloc MQTT.
// ════════════════════════════════════════════════════════════

namespace page_net {

// -- géométrie --
constexpr int CARD_Y   = SI_PAGE_Y0 + 32;
constexpr int CARD_X   = SI_LX - 2;
constexpr int CARD_W   = SI_W - CARD_X * 2;
constexpr int CARD_TH  = 11;
constexpr int CARD_H   = CARD_TH + 22;
constexpr int VAL_Y    = CARD_Y + CARD_TH + 4;
constexpr int GAU_X    = 108;
constexpr int GAU_W    = 90;

constexpr int GX[]     = { SI_LX, 164 };   // grille d'adressage
constexpr int GW       = 148;

// RSSI -> fraction de jauge. -90 dBm = plancher exploitable, -30 = collé à l'AP.
static float rssi_frac(int rssi) {
    return constrain((rssi + 90) / 60.0f, 0.0f, 1.0f);
}

static const char* rssi_label(int rssi) {
    if (rssi > -60) return "EXCELLENT";
    if (rssi > -70) return "BON";
    if (rssi > -80) return "FAIBLE";
    return "CRITIQUE";
}

static void cell(int col, int y, const char* key, const char* val, uint16_t c) {
    draw::text(GX[col], y, key, C_DKCYAN);
    draw::text_right(GX[col] + GW, y, val, c);
}

static void draw() {
    char buf[48];
    int  y = SI_PAGE_Y0;

    draw::section(y, ">> RESEAU");

    // -- pastilles d'état, avant toute lecture --
    bool wifiUp = wifi_is_connected();
    bool mqttUp = mqtt_is_connected();
    int  bx = SI_LX;
    bx += draw::badge(bx, SI_PAGE_Y0 + 16, wifiUp ? "WIFI  CONNECTE" : "WIFI  DECONNECTE",
                      wifiUp ? C_GREEN : C_RED, wifiUp ? C_BG : C_WHITE) + 8;
    draw::badge(bx, SI_PAGE_Y0 + 16, mqttUp ? "MQTT  CONNECTE" : "MQTT  DECONNECTE",
                mqttUp ? C_GREEN : C_RED, mqttUp ? C_BG : C_WHITE);

    if (!wifiUp) {
        // Sprite = police GFX ASCII pure : pas d'accent ni de cadratin ici.
        draw::text(SI_LX, CARD_Y + 6, "Pas d'association WiFi - adressage indisponible.", C_DIM);
        return;
    }

    // -- carte SIGNAL --
    int      rssi = WiFi.RSSI();
    uint16_t rc   = rssi > -60 ? C_GREEN : rssi > -75 ? C_YELLOW : C_RED;

    surface::rect(CARD_X, CARD_Y, CARD_W, CARD_H, C_DKCYAN);
    surface::fill_rect(CARD_X + 1, CARD_Y + 1, CARD_W - 2, CARD_TH - 1, C_DKCYAN);
    draw::text(CARD_X + 4, CARD_Y + 3, "SIGNAL", C_WHITE, C_DKCYAN);

    snprintf(buf, sizeof(buf), "%d", rssi);
    draw::big(SI_LX + 2, VAL_Y, buf, rc);
    draw::text(SI_LX + 2 + (int)strlen(buf) * 12 + 4, VAL_Y + 8, "dBm", C_DIM);
    draw::bar(GAU_X, VAL_Y + 3, GAU_W, draw::BAR_HERO, rssi_frac(rssi), rc);
    draw::text(GAU_X + GAU_W + 8, VAL_Y + 4, rssi_label(rssi), rc);

    y = CARD_Y + CARD_H + 6;
    draw::hline(y); y += 5;
    draw::section(y, ">> ADRESSAGE"); y += SI_LH + 2;

    // SSID tronqué : valeur alignée à droite, un nom long chevaucherait son
    // propre intitulé.
    snprintf(buf, sizeof(buf), "%.18s", WiFi.SSID().c_str());
    cell(0, y, "SSID", buf, C_WHITE);
    snprintf(buf, sizeof(buf), "canal %d", WiFi.channel());
    cell(1, y, "Radio", buf, C_WHITE);
    y += SI_LH;

    cell(0, y, "IP",         WiFi.localIP().toString().c_str(),   C_GREEN);
    cell(1, y, "Passerelle", WiFi.gatewayIP().toString().c_str(), C_WHITE);
    y += SI_LH;

    cell(0, y, "Masque", WiFi.subnetMask().toString().c_str(), C_WHITE);
    cell(1, y, "DNS",    WiFi.dnsIP().toString().c_str(),      C_WHITE);
    y += SI_LH;

    cell(0, y, "Hote", WIFI_HOSTNAME,             C_WHITE);
    cell(1, y, "MAC",  WiFi.macAddress().c_str(), C_DIM);
    y += SI_LH + 4;

    draw::hline(y); y += 5;
    draw::section(y, ">> MQTT"); y += SI_LH + 2;

    if (mqttUp) {
        snprintf(buf, sizeof(buf), "%s:%d", MQTT_BROKER, MQTT_PORT);
        draw::pair(SI_LX, y, "Broker    : ", buf,            C_GREEN); y += SI_LH;
        draw::pair(SI_LX, y, "Client ID : ", MQTT_CLIENT_ID, C_WHITE);
    } else {
        snprintf(buf, sizeof(buf), "%s:%d", MQTT_BROKER, MQTT_PORT);
        draw::pair(SI_LX, y, "Broker    : ", buf,        C_DIM); y += SI_LH;
        draw::pair(SI_LX, y, "Etat      : ", "DECONNECTE", C_RED);
    }
}

}  // namespace page_net


// ════════════════════════════════════════════════════════════
// PAGE 7 — CAPTEUR (APDS-9930)
// ════════════════════════════════════════════════════════════
// Page de RÉGLAGE : light_manager y compte les gestes sans les transmettre à
// l'IA (sysinfo_sensor_page_active), on cale donc les seuils en regardant la
// jauge. La proximité est rafraîchie 5x plus vite que le reste — c'est la seule
// valeur avec laquelle on interagit à la main.

namespace page_light {

// -- géométrie --
constexpr int BADGE_Y = SI_PAGE_Y0 + 16;
constexpr int CARD_Y  = BADGE_Y + 16;
constexpr int CARD_X  = SI_LX - 2;
constexpr int CARD_W  = SI_W - CARD_X * 2;
constexpr int CARD_TH = 11;
constexpr int CARD_H  = CARD_TH + 36;   // vedette + jauge + légende
constexpr int VAL_Y   = CARD_Y + CARD_TH + 5;
constexpr int LEG_Y   = VAL_Y + 18;     // sous la vedette (double hauteur) et la jauge
constexpr int GAU_X   = SI_LX + 86;
constexpr int GAU_W   = SI_W - GAU_X - SI_LX - 2;

constexpr int RULE1_Y = CARD_Y + CARD_H + 4;
constexpr int SEC1_Y  = RULE1_Y + 5;
constexpr int ROW1_Y  = SEC1_Y + SI_LH + 2;
constexpr int ROW1_N  = 3;

constexpr int RULE2_Y = ROW1_Y + ROW1_N * SI_LH + 3;
constexpr int SEC2_Y  = RULE2_Y + 5;
constexpr int ROW2_Y  = SEC2_Y + SI_LH + 2;
constexpr int ROW2_N  = 3;

// Bandes BLITTÉES : la proximité seule d'un côté, les mesures lentes de l'autre.
constexpr int LIVE_P_Y = BADGE_Y - 1;
constexpr int LIVE_P_H = CARD_Y + CARD_H - LIVE_P_Y;
constexpr int LIVE_S_Y = ROW1_Y - 1;
constexpr int LIVE_S_H = ROW1_N * SI_LH + 1;
constexpr int LIVE_T_Y = ROW2_Y - 1;
constexpr int LIVE_T_H = ROW2_N * SI_LH + 1;

static_assert(ROW2_Y + ROW2_N * SI_LH <= SI_PAGE_YMAX,
              "page CAPTEUR : la derniere ligne deborde sur le pied de page");

constexpr int PROX_MAX = 1023;   // pleine échelle du canal proximité

// Un poll sur EVERY_N rafraîchit aussi les bandes lentes.
constexpr int SLOW_EVERY_N = 5;
static int _slow = 0;

// ---- BLITTÉ ----

// Repère de seuil sur la jauge de proximité : c'est lui qu'on vient lire pour
// décider si LIGHT_PROX_NEAR_DELTA/FAR_DELTA sont bien placés.
static void mark(int thr, uint16_t c) {
    int x = GAU_X + 1 + (int)((GAU_W - 2) * (float)thr / PROX_MAX);
    surface::vline(x, VAL_Y + 1, draw::BAR_HERO - 2, c);
}

static void draw_prox(const LightStatus& s) {
    char buf[40];

    surface::fill_rect(0, LIVE_P_Y, SI_W, LIVE_P_H, C_BG);

    int bx = SI_LX;
    bx += draw::badge(bx, BADGE_Y, s.present ? "CAPTEUR DETECTE" : "CAPTEUR ABSENT",
                      s.present ? C_GREEN : C_RED, s.present ? C_BG : C_WHITE) + 6;
    bx += draw::badge(bx, BADGE_Y, "GESTE INHIBE", C_YELLOW) + 6;
    // Un étage saturé rend une valeur d'allure normale : seul ce badge le dit.
    if (s.psat)   bx += draw::badge(bx, BADGE_Y, "SATURE", C_RED, C_WHITE) + 6;
    if (s.asleep) draw::badge(bx, BADGE_Y, "VEILLE", C_MAGENTA, C_WHITE);

    surface::rect(CARD_X, CARD_Y, CARD_W, CARD_H, C_DKCYAN);
    surface::fill_rect(CARD_X + 1, CARD_Y + 1, CARD_W - 2, CARD_TH - 1, C_DKCYAN);
    draw::text(CARD_X + 4, CARD_Y + 3, "PROXIMITE", C_WHITE, C_DKCYAN);

    if (!s.present) {
        draw::text(SI_LX + 2, VAL_Y + 4, "Aucune reponse en 0x39 - branchez le capteur.", C_DIM);
        return;
    }

    uint16_t c = s.near ? C_GREEN : s.prox > s.thr_far ? C_YELLOW : C_DIM;
    snprintf(buf, sizeof(buf), "%4u", (unsigned)s.prox);
    draw::big(SI_LX + 2, VAL_Y, buf, c);
    draw::text(SI_LX + 2 + 4 * 12 + 4, VAL_Y + 8, "prox", C_DIM);

    draw::bar(GAU_X, VAL_Y + 1, GAU_W, draw::BAR_HERO, (float)s.prox / PROX_MAX, c);
    mark(s.prox_base, C_CYAN);     // repos suivi, d'où partent les deux seuils
    mark(s.thr_far,   C_ORANGE);
    mark(s.thr_near,  C_RED);

    // Légende cadrée SUR LA JAUGE, pas sur la valeur-vedette, et dans l'ordre où
    // les repères y apparaissent. Chaque valeur porte la couleur de SON trait.
    snprintf(buf, sizeof(buf), "repos %u", (unsigned)s.prox_base);
    draw::text(GAU_X, LEG_Y, buf, C_CYAN);
    snprintf(buf, sizeof(buf), "proche %u", (unsigned)s.thr_near);
    draw::text_right(GAU_X + GAU_W, LEG_Y, buf, C_RED);
    snprintf(buf, sizeof(buf), "loin %u", (unsigned)s.thr_far);
    draw::text(GAU_X + (GAU_W - gfx::text_w(buf)) / 2, LEG_Y, buf, C_ORANGE);
}

static void draw_slow(const LightStatus& s) {
    char buf[48];
    char aux[24];

    surface::fill_rect(0, LIVE_S_Y, SI_W, LIVE_S_H, C_BG);
    surface::fill_rect(0, LIVE_T_Y, SI_W, LIVE_T_H, C_BG);

    // -- mesures --
    int y = ROW1_Y;
    snprintf(buf, sizeof(buf), "%lu.%02lu lux",
             (unsigned long)(s.clux / 100), (unsigned long)(s.clux % 100));
    draw::row(y, "Lumiere ", buf, C_CYAN, true,
              (float)s.clux / LIGHT_CLUX_BRIGHT, C_CYAN, 120);
    y += SI_LH;

    snprintf(buf, sizeof(buf), "ch0 %u   ch1 %u", (unsigned)s.ch0, (unsigned)s.ch1);
    draw::row(y, "Canaux  ", buf, C_WHITE);
    y += SI_LH;

    snprintf(buf, sizeof(buf), "%d%%", s.brightness);
    draw::row(y, "Ecran   ", buf, C_GREEN, true, s.brightness / 100.0f, C_GREEN, 120);
    draw::text_right(SI_W - SI_LX, y,
                     !s.auto_on ? "MANUEL" : s.manual_hold ? "AUTO (suspendu)" : "AUTO",
                     s.auto_on && !s.manual_hold ? C_GREEN : C_YELLOW);

    // -- geste et veille --
    y = ROW2_Y;
    if (s.gestures) snprintf(aux, sizeof(aux), "il y a %lus",
                             (unsigned long)(s.since_gesture / 1000));
    else            snprintf(aux, sizeof(aux), "aucun");
    snprintf(buf, sizeof(buf), "%lu   (%s)", (unsigned long)s.gestures, aux);
    draw::row(y, "Gestes  ", buf, s.gestures ? C_GREEN : C_DIM);
    y += SI_LH;

    if (s.since_present < LIGHT_POLL_MS * 5) snprintf(buf, sizeof(buf), "OUI");
    else snprintf(buf, sizeof(buf), "non   (il y a %lus)",
                  (unsigned long)(s.since_present / 1000));
    draw::row(y, "Presence", buf, s.since_present < LIGHT_POLL_MS * 5 ? C_GREEN : C_DIM);
    y += SI_LH;

    if (s.asleep) snprintf(buf, sizeof(buf), "ACTIVE");
    else if (s.since_present >= LIGHT_SLEEP_TIMEOUT_MS) snprintf(buf, sizeof(buf), "imminente");
    else snprintf(buf, sizeof(buf), "dans %lus",
                  (unsigned long)((LIGHT_SLEEP_TIMEOUT_MS - s.since_present) / 1000));
    draw::row(y, "Veille  ", buf, s.asleep ? C_MAGENTA : C_WHITE);
    snprintf(buf, sizeof(buf), "%lu branchement(s)", (unsigned long)s.plugs);
    draw::text_right(SI_W - SI_LX, y, buf, C_DIM);
}

static void refresh() {
    LightStatus s;
    light_get_status(&s);

    draw_prox(s);
    surface::blit_rows(LIVE_P_Y, LIVE_P_H);

    if (++_slow < SLOW_EVERY_N) return;
    _slow = 0;
    draw_slow(s);
    surface::blit_rows(LIVE_S_Y, LIVE_S_H);
    surface::blit_rows(LIVE_T_Y, LIVE_T_H);
}

// ---- FIXE ----

static void draw() {
    draw::section(SI_PAGE_Y0, ">> CAPTEUR APDS-9930");
    draw::text_right(SI_W - SI_LX, SI_PAGE_Y0, "I2C 0x39 - bus tactile", C_DIM);

    draw::hline(RULE1_Y);
    draw::section(SEC1_Y, ">> MESURES");
    draw::hline(RULE2_Y);
    draw::section(SEC2_Y, ">> GESTE ET VEILLE");

    _slow = SLOW_EVERY_N - 1;   // la 1re passe remplit les deux bandes lentes

    LightStatus s;
    light_get_status(&s);
    draw_prox(s);
    draw_slow(s);
}

}  // namespace page_light


// ════════════════════════════════════════════════════════════
// TABLE DES PAGES
// ════════════════════════════════════════════════════════════
// Source unique de l'ordre, du rendu et de la cadence. Ajouter une page =
// une ligne ici + un bloc namespace ci-dessus.

struct PageDesc {
    void  (*draw)();      // rendu complet dans le sprite
    void  (*refresh)();   // colonnes vivantes + blit partiel ; nullptr = statique
    uint8_t period;       // ticks de 50 ms entre deux appels à refresh
};

static const PageDesc _pages[] = {
    { page_chip::draw,  page_chip::refresh,  SI_REFRESH_TICKS                     },
    { page_mem::draw,   page_mem::refresh,   SI_REFRESH_TICKS * page_mem::EVERY_N },
    { page_tasks::draw, page_tasks::refresh, SI_REFRESH_TICKS                     },
    { page_part::draw,  nullptr,             0                                    },
    { page_fs::draw,    nullptr,             0                                    },
    { page_net::draw,   nullptr,             0                                    },
    { page_light::draw, page_light::refresh, SI_SENSOR_TICKS                      },
};

static const int _page_count = sizeof(_pages) / sizeof(_pages[0]);

static_assert(sizeof(_pages) / sizeof(_pages[0]) == SYSINFO_PAGE_COUNT,
              "SYSINFO_PAGE_COUNT (sysinfo_manager.h) doit suivre la table _pages[]");

static int _page = 0;   // page courante


// ════════════════════════════════════════════════════════════
// TICKER — horodatage 20 Hz + rafraîchissements à la période de la page
// ════════════════════════════════════════════════════════════
// ⚠️ %CPU mesuré ENTRE DEUX RENDUS : le coût du rendu compte donc dans
// loopTask, qui apparaît plus chargée quand on la regarde. Même biais que htop.

namespace ticker {

static lv_timer_t* _timer = nullptr;
static uint8_t     _ticks = 0;

// ⚠️ Doit être appelé sur TOUTE sortie d'écran, pas seulement par screen::hide :
// display_show_home/nas/ai() font un lv_scr_load() nu. Un timer survivant blitte
// dans un canvas invisible ET un second est créé au retour, ce qui fait avancer
// le compteur de ticks deux fois trop vite. Idempotent.
static void stop() {
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

static void cb(lv_timer_t* t) {
    frame::clock();
    frame::blit_clock();

    const PageDesc& p = _pages[_page];
    if (!p.refresh) return;                       // page statique
    if (++_ticks < p.period) return;
    _ticks = 0;
    p.refresh();
}

static void start() {
    stop();
    _ticks = 0;
    _timer = lv_timer_create(cb, 50, nullptr);
}

// Changement de page : la cadence repart de zéro.
static void reset_cadence() { _ticks = 0; }

}  // namespace ticker


// ════════════════════════════════════════════════════════════
// SCREEN — écran LVGL, canvas, navigation tactile
// ════════════════════════════════════════════════════════════

namespace screen {

static lv_obj_t* obj           = nullptr;   // écran LVGL dédié (créé une fois)
static lv_obj_t* return_screen = nullptr;   // écran à restaurer à la sortie

// Rendu complet d'une page, décor compris. Utilisé à l'entrée sur l'écran.
static void full_redraw() {
    draw::fill_bg();
    frame::header(_page);
    frame::footer();
    frame::clear_content();
    _pages[_page].draw();
    surface::blit();
}

// Changement de page : le fond et le pied ne bougent pas.
static void redraw_page() {
    frame::header(_page);
    frame::clear_content();
    _pages[_page].draw();
    surface::blit();
}

static void goto_page(int page) {
    _page = ((page % _page_count) + _page_count) % _page_count;
    ticker::reset_cadence();
    redraw_page();
}

static void hide() {
    ticker::stop();
    if (return_screen) lv_scr_load(return_screen);
}

// Couvre TOUTES les sorties d'écran, y compris celles qui ne passent pas par
// hide() (page:home, bascule sur l'écran IA au réveil).
static void unloaded_cb(lv_event_t* e) {
    ticker::stop();
}

// Tap : gauche = page précédente, droite = suivante, centre = sortie.
static void click_cb(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if      (p.x < 110) goto_page(_page - 1);
    else if (p.x > 210) goto_page(_page + 1);
    else                hide();
}

// Créé une seule fois, à la première ouverture de l'écran, et jamais rendu.
// Coût : ~311 Ko de PSRAM et ~10 Ko de RAM interne (les objets LVGL).
static void ensure_created() {
    if (obj) return;

    surface::buf = (uint16_t*)heap_caps_malloc((size_t)SI_W * SI_H * 2, MALLOC_CAP_SPIRAM);
    if (!surface::buf) {
        log_line("[SysInfo] PSRAM KO pour la surface (%u o) — écran indisponible",
                 (unsigned)((size_t)SI_W * SI_H * 2));
        return;
    }
    surface::cv.px = surface::buf;

    surface::canvas_buf =
        (lv_color_t*)heap_caps_malloc((size_t)SI_W * SI_H * 2, MALLOC_CAP_SPIRAM);
    if (!surface::canvas_buf) {
        // On renonce à l'écran plutôt que de déréférencer nullptr.
        log_line("[SysInfo] PSRAM KO pour le canvas (%u o) — écran indisponible",
                 (unsigned)((size_t)SI_W * SI_H * 2));
        free(surface::buf);
        surface::buf   = nullptr;
        surface::cv.px = nullptr;
        return;
    }

    obj = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, nullptr);

    surface::canvas = lv_canvas_create(obj);
    lv_canvas_set_buffer(surface::canvas, surface::canvas_buf, SI_W, SI_H,
                         LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_obj_set_pos(surface::canvas, 0, 0);
    lv_obj_add_flag(surface::canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(surface::canvas, click_cb, LV_EVENT_CLICKED, nullptr);
}

static bool is_active() { return obj && lv_scr_act() == obj; }

}  // namespace screen


// ---- API PUBLIQUES ----

bool sysinfo_sensor_page_active() {
    return screen::is_active() && _page == SYSINFO_PAGE_LIGHT;
}

// Séparateur titré à largeur fixe : "=== TITRE ===…===(NN% libre)".
static void _log_sep(const char* title, unsigned pctFree) {
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
static void _log_sep_plain() {
    char line[46];
    memset(line, '=', 45);
    line[45] = '\0';
    log_line("[MEM] %s", line);
}

// Journalise l'état mémoire (cmd "mem"). En OCTETS : le pire cas interne se
// joue à quelques Ko près, l'arrondi Ko de la page MÉMOIRE est trop grossier.
void sysinfo_log_memory() {
    inv::Mem m = inv::read();

    unsigned intPct = m.int_total ? (unsigned)(100u * m.int_free / m.int_total) : 0;
    unsigned psPct  = m.ps_total  ? (unsigned)(100u * m.ps_free  / m.ps_total)  : 0;

    TaskStatus_t st[SI_MAX_TASKS];
    UBaseType_t  n = uxTaskGetSystemState(st, SI_MAX_TASKS, nullptr);

    // === RAM (interne) : postes → bilan → marges ===
    // int_used = postes connus + ESP_SR + reste non tracé (WiFi/lwIP, cœur…).
    _log_sep("HEAP", intPct);
    if (n == 0) {
        log_line("[MEM] %-20s : uxTaskGetSystemState a échoué", "Piles");
    } else {
        // High-water : le plus petit reste JAMAIS atteint depuis le boot.
        for (auto const& s : inv::stacks) {
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
    for (auto const& a : inv::allocs)
        if (a.internal) log_line("[MEM] Buffer %-13s : %u o", a.label, (unsigned)a.size());
    log_line("[MEM] %-20s : %u o d'interne (mesuré au boot)", "ESP_SR utilise", (unsigned)m.esp_sr);
    log_line("[MEM] %-22s : %ld o", "Système (non tracé)", m.untracked_int);
    log_line("[MEM] %-20s : %u o utilise / %u o", "   Bilan HEAP", (unsigned)m.int_used, (unsigned)m.int_total);
    log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "Interne", (unsigned)m.int_free, (unsigned)m.int_min);
    log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "DMA", (unsigned)m.dma_free, (unsigned)m.dma_min);
    // Un total libre confortable ne garantit pas qu'une alloc d'un seul tenant passe.
    log_line("[MEM] %-20s : %u o", "Plus gros bloc libre", (unsigned)m.int_largest);

    // === PSRAM : postes → bilan → marge ===
    if (m.has_ps) {
        _log_sep("PSRAM", psPct);
        for (auto const& a : inv::allocs) {
            if (a.internal) continue;
            // Buffers alloués paresseusement : "(pas encore calculé)" plutôt
            // que "0 o", et exclus du total tant qu'ils n'existent pas.
            bool pending = inv::alloc_pending(a);
            if (pending) log_line("[MEM] Buffer %-13s : (pas encore calculé)", a.label);
            else         log_line("[MEM] Buffer %-13s : %u o", a.label, (unsigned)a.size());
        }
        // Non tracé : modèles ESP_SR chargés en PSRAM + framework.
        log_line("[MEM] %-22s : %ld o", "Système (non tracé)", m.untracked_ps);
        log_line("[MEM] %-20s : %u o utilisés / %u o", "   Bilan PSRAM",
                 (unsigned)(m.ps_total - m.ps_free), (unsigned)m.ps_total);
        log_line("[MEM] %-20s : %u o libre (pire cas : %u o)", "PSRAM",
                 (unsigned)m.ps_free, (unsigned)m.ps_min);
        log_line("[MEM] %-20s : %u o", "Plus gros bloc libre", (unsigned)m.ps_largest);
    } else {
        _log_sep("PSRAM", 0);
        log_line("[MEM] %-20s : NON DETECTEE", "PSRAM");
    }

    _log_sep_plain();   // ferme le dernier bloc
}

void display_show_sysinfo() {
    if (screen::is_active()) {
        // Déjà affiché : le rappel sert alors à parcourir les pages.
        screen::goto_page(_page + 1);
        return;
    }
    display_show_sysinfo_page(0);
}

void display_show_sysinfo_page(int page) {
    if (page < 0 || page >= _page_count) {
        log_line("[SysInfo] page hors plage : %d (0-%d)", page, _page_count - 1);
        return;
    }

    if (screen::is_active()) {
        // Écran déjà en place : on ne fait que changer de page.
        if (page != _page) screen::goto_page(page);
        return;
    }

    screen::ensure_created();
    if (!screen::obj) return;   // allocation PSRAM échouée

    screen::return_screen = lv_scr_act();
    _page = page;

    lv_scr_load(screen::obj);
    screen::full_redraw();

    ticker::start();
}
