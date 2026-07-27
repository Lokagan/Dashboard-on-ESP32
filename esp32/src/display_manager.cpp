// ============================================================
// DISPLAY_MANAGER.CPP — LVGL 9.x + TFT_eSPI (ILI9341)
// TFT_eSPI initialise l'écran, LVGL lui est relié par _lv_flush_cb.
//
// SquareLine est la source de vérité de l'UI : on ne touche ici qu'aux
// VALEURS (textes, séries, plages), jamais à la structure ni à la
// géométrie des objets générés. Les corrections de mise en page se font
// dans SquareLine Studio.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <string.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "../squareline/ui/ui.h"
#include "display_manager.h"
#include "sysinfo_manager.h"
#include "audio_manager.h"
#include "ai_manager.h"
#include "ai_companion.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----

// newlib nano ne supporte pas %f dans lv_label_set_text_fmt
#define FLOAT_INT(v)  ((int)(v))
#define FLOAT_DEC(v)  ((int)(((v) - (int)(v)) * 10 + 0.5f) % 10)

// LV_BUF_LINES/LV_BUF_BYTES et les JSON_*_SIZE vivent dans display_manager.h :
// la page MEMOIRE de SysInfo les lit aussi (_si_alloc), et la ligne LVGL de sa
// table avait déjà divergé de 3 840 o.

// Cadence max de mise à jour des charts. Les données arrivent par rafales
// toutes les ~5 s : traitées une par frame, elles provoquaient 6 à 8 repeints
// au lieu d'un, chacun sur ~75 % de l'écran (ext_draw_size des scales).
#define CHART_REFRESH_MIN_INTERVAL_MS 1000

// Instance partagée (extern depuis sysinfo_manager.cpp)
TFT_eSPI _tft;

static lv_display_t* _disp = nullptr;
static lv_color_t*   _buf1 = nullptr;
static lv_color_t*   _buf2 = nullptr;   // 2e buffer LVGL — cf. display_init()
static unsigned long _last_lv_tick = 0;

static lv_chart_series_t* _chart_nas_cpu   = nullptr;
static lv_chart_series_t* _chart_nas_ram   = nullptr;
static lv_chart_series_t* _chart_nas_read  = nullptr;
static lv_chart_series_t* _chart_nas_write = nullptr;
static lv_chart_series_t* _chart_nas_net_in  = nullptr;   // réseau IN/OUT sur l'axe droit
static lv_chart_series_t* _chart_nas_net_out = nullptr;   // (débit MB/s, avec R/W)
static lv_chart_series_t* _chart_fb_down   = nullptr;
static lv_chart_series_t* _chart_fb_up     = nullptr;

// Les charts sont créés en CODE (plus dans SquareLine — cf. _chart_build), comme
// les tables. Objets + labels d'échelle min/mid/max faits main, l'échelle
// SquareLine étant partie avec le widget.
static lv_obj_t* _chart_nas = nullptr;
static lv_obj_t* _chart_fb  = nullptr;

// Labels d'échelle. Index : [0]=max (haut), [1]=mid, [2]="0" (bas, figé).
static lv_obj_t* _nas_lblL[3] = {nullptr, nullptr, nullptr};   // gauche : CPU/RAM %
static lv_obj_t* _nas_lblR[3] = {nullptr, nullptr, nullptr};   // droite : R/W (×10 -> /10)
static lv_obj_t* _fb_lblL[3]  = {nullptr, nullptr, nullptr};   // gauche : RX (×10 -> /10)
static lv_obj_t* _fb_lblR[3]  = {nullptr, nullptr, nullptr};   // droite : TX (×10 -> /10)

// Dernière plage appliquée par axe : les labels ne sont réécrits qu'au changement
// (lv_label_set_text invalide même à texte identique — règle du projet).
static int32_t _nas_rangeL = -1, _nas_rangeR = -1;
static int32_t _fb_rangeL  = -1, _fb_rangeR  = -1;

static bool _nas_chart_dirty = false;
static bool _fb_chart_dirty  = false;
static unsigned long _last_chart_refresh = 0;

// Cache des dernières valeurs reçues, tenu à jour quel que soit l'écran actif :
// garde les charts continus et permet de rattraper l'affichage à l'entrée sur
// l'écran, sans attendre le prochain message MQTT.
static struct {
    int   cpu = 0;
    int   ram = 0;
    float temp = 0;
    int   net_in = 0;
    int   net_out = 0;
    int   vol_pct = 0;
    char  vol_status[32] = "--";
    float vol_read = 0;
    float vol_write = 0;
} _nas_state;

static struct {
    float down = 0;
    float up = 0;
    int   bw_down = 0;
    int   bw_up = 0;
    char  state[32] = "--";
    char  ipv4[32]  = "--";
    int   dev_active = 0;
    int   dev_total = 0;
} _fb_state;

// Points de chart EN ATTENTE — déposés par les display_update_*, dépilés tous
// dans UNE frame par display_loop (cf. CHART_REFRESH_MIN_INTERVAL_MS). Aucun
// point perdu : le dépilage tourne bien plus vite que la publication (~5 s).
static struct {
    bool  has_cpu = false, has_ram = false, has_read = false, has_write = false;
    bool  has_net_in = false, has_net_out = false;
    int   cpu = 0, ram = 0;
    float read = 0, write = 0;
    int   net_in = 0, net_out = 0;   // kB/s bruts, convertis en MB/s×10 au dépilage
} _nas_chart_pending;

static struct {
    bool  has_down = false, has_up = false;
    float down = 0, up = 0;
} _fb_chart_pending;

static char* _json_nas_disks       = nullptr;
static char* _json_nas_downloads   = nullptr;
static char* _json_nas_connections = nullptr;
static char* _json_fbx_devices     = nullptr;

// Source courante affichée dans ScreenTable
typedef enum {
    TABLE_NAS_DISKS,
    TABLE_NAS_DOWNLOADS,
    TABLE_NAS_CONNECTIONS,
    TABLE_FBX_DEVICES,
    TABLE_FBX_ACTIVITY,
    TABLE_SOURCE_COUNT
} TableSource;

static TableSource _table_source = TABLE_NAS_DISKS;

// Source dont la structure est en place dans _lv_table (COUNT = aucune)
static TableSource _table_built_source = TABLE_SOURCE_COUNT;

static lv_obj_t* _lv_table          = nullptr;
static lv_obj_t* _title_label       = nullptr;   // titre de ScreenTable, créé une fois
static lv_obj_t* _table_back_screen = nullptr;   // écran d'où l'on vient

// Retour auto de l'écran IA vers l'écran précédent (cf. display_show_ai / display_loop).
static lv_obj_t*     _ai_back_screen      = nullptr;  // écran d'où le vocal/MQTT a ouvert l'IA
static unsigned long _ai_last_activity_ms = 0;        // dernier changement d'état IA = activité
static bool          _ai_return_armed     = false;    // armé à l'entrée vocale/MQTT, pas en nav manuelle

static bool _display_paused  = false;
static volatile bool _force_redraw = false;
static bool _ai_was_speaking = false;
static bool _ota_screen_init = false;   // cf. display_show_ota_progress

// Vrai pendant lv_timer_handler(), donc pendant les écritures SPI de
// _lv_flush_cb. Lu depuis ota_task : volatile obligatoire (l'autre cœur ne
// verrait sinon jamais le changement, le compilateur gardant la valeur en
// registre). Un octet écrit/lu par un seul écrivain — pas besoin de mutex.
static volatile bool _in_lvgl = false;
#define DISPLAY_PAUSE_TIMEOUT_MS 1000

// Instrumentation du rendu, relevée avec les "slow frame" : distingue un
// redessin plein écran (chercher QUI invalide) d'un petit nombre de pixels
// coûteux (le temps est dans le rendu, pas dans le SPI).
static uint16_t _flush_count = 0;
static uint32_t _flush_px    = 0;
static int16_t  _flush_x1 = 0, _flush_y1 = 0, _flush_x2 = 0, _flush_y2 = 0;

// Espion d'invalidation, armé par display_spy_invalidations() (cmd "spy").
// Les "slow frame" ne donnent que l'UNION des zones ; LV_EVENT_INVALIDATE_AREA
// donne chaque rectangle AVANT fusion, donc le widget responsable.
static volatile uint8_t _spy_remaining = 0;

// ---- HELPERS ----

// --- Charts faits main (cf. déclarations plus haut) ---
// Géométrie reprise de l'ancien widget SquareLine (280×95 en (20,110)).
#define CHART_X       20
#define CHART_Y       110
#define CHART_W       280
#define CHART_H       95
#define CHART_POINTS  30

// Palette métier — chaque métrique une couleur, portée par son readout ET sa
// courbe ET son label d'échelle. ⚠️ DOIT rester alignée sur les couleurs des
// readouts dans SquareLine (ui_ScreenNAS/Freebox.c) : si l'une change là-bas,
// la mettre à jour ici (paire SquareLine/code, comme ailleurs dans le projet).
#define COL_CPU    lv_color_hex(0xAAFF00)
#define COL_RAM    lv_color_hex(0x0088FF)
#define COL_READ   lv_color_hex(0x00FF00)
#define COL_WRITE  lv_color_hex(0xFF0000)
#define COL_RX     lv_color_hex(0x00FFFF)
#define COL_TX     lv_color_hex(0xFF8800)

// Moteur générique : crée un chart (géométrie + style + 6 labels d'échelle) sur
// `screen`, remplit lblL/lblR ([0]=max, [1]=mid, [2]="0" figé), colore les labels
// par côté. Les SÉRIES sont ajoutées par l'appelant (noms référencés ailleurs).
// Factorise ce qui était dupliqué NAS/Freebox — esprit du moteur de tables.
static lv_obj_t* _chart_build(lv_obj_t* screen, lv_color_t colL, lv_color_t colR,
                              lv_obj_t* lblL[3], lv_obj_t* lblR[3]) {
    lv_obj_t* ch = lv_chart_create(screen);
    lv_obj_set_pos(ch, CHART_X, CHART_Y);
    lv_obj_set_size(ch, CHART_W, CHART_H);
    lv_obj_remove_flag(ch, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));  // OR = int en C++, cast requis
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, CHART_POINTS);
    lv_chart_set_div_line_count(ch, 3, 0);            // 3 horizontales = max/mid/min (LVGL les place à 0, h/2, h), 0 verticale
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y,   0, 10);
    lv_chart_set_range(ch, LV_CHART_AXIS_SECONDARY_Y, 0, 10);

    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, LV_PART_MAIN);   // fond = écran
    lv_obj_set_style_border_width(ch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ch, 1, LV_PART_MAIN);  // 1 px : sinon la div line MIN (y=h) tombe hors zone et est clippée
    lv_obj_set_style_line_color(ch, lv_color_hex(0x383838), LV_PART_MAIN);  // grille dim
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);        // PAS de pastilles (perf + lisibilité)
    lv_obj_set_style_line_width(ch, 3, LV_PART_ITEMS);         // épaisseur des courbes
    lv_obj_set_style_line_opa(ch, LV_OPA_80, LV_PART_ITEMS);   // ~80% : lisibilité des croisements sans ternir

    // 6 labels d'échelle dans les marges de ~20 px. Bas figé à "0" (plage 0..max).
    const int ys[3]  = { CHART_Y - 4, CHART_Y + CHART_H / 2 - 4, CHART_Y + CHART_H - 8 };
    const int xR     = CHART_X + CHART_W + 2;
    for (int i = 0; i < 3; i++) {
        lblL[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(lblL[i], &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_color(lblL[i], colL, 0);
        lv_obj_set_pos(lblL[i], 0, ys[i]);
        lv_label_set_text(lblL[i], "0");

        lblR[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(lblR[i], &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_color(lblR[i], colR, 0);
        lv_obj_set_pos(lblR[i], xR, ys[i]);
        lv_label_set_text(lblR[i], "0");
    }
    return ch;
}

// Max des N séries d'un axe (tableau, façon _table_fill), +5 % de marge. Plancher 10.
static int32_t _chart_axis_range(lv_obj_t* ch, lv_chart_series_t* const* series, uint8_t n) {
    uint16_t nb = lv_chart_get_point_count(ch);
    int32_t mx = 10;
    for (uint8_t s = 0; s < n; s++) {
        lv_coord_t* d = lv_chart_get_y_array(ch, series[s]);
        for (uint16_t i = 0; i < nb; i++)
            if (d[i] != LV_CHART_POINT_NONE && d[i] > mx) mx = d[i];
    }
    return mx + mx / 20;
}

// Format d'un label d'échelle : entier si div<=1 (%), sinon X.Y (valeur ×10).
static void _chart_scale_fmt(char* buf, size_t n, int32_t v, uint8_t div) {
    if (div <= 1) snprintf(buf, n, "%ld", (long)v);
    else          snprintf(buf, n, "%ld.%ld", (long)(v / div), (long)(v % div));
}

// Recale un axe + réécrit ses labels max/mid — mais SEULEMENT si la plage a bougé.
static void _chart_axis_update(lv_obj_t* ch, lv_chart_axis_t axis,
                               lv_chart_series_t* const* series, uint8_t n,
                               lv_obj_t* lbl[3], int32_t& cache, uint8_t div) {
    int32_t rm = _chart_axis_range(ch, series, n);
    lv_chart_set_range(ch, axis, 0, rm);
    if (rm == cache) return;
    cache = rm;
    char buf[12];
    _chart_scale_fmt(buf, sizeof(buf), rm,     div); lv_label_set_text(lbl[0], buf);
    _chart_scale_fmt(buf, sizeof(buf), rm / 2, div); lv_label_set_text(lbl[1], buf);
}

// NAS : gauche % CPU/RAM (div 1) ; droite MB/s = R/W disque + réseau IN/OUT (div 10).
// Freebox : un seul côté chacun (×10). Ajouter une série = une entrée de tableau.
static void _refresh_chart_nas() {
    lv_chart_series_t* prim[] = { _chart_nas_cpu, _chart_nas_ram };
    lv_chart_series_t* sec[]  = { _chart_nas_read, _chart_nas_write, _chart_nas_net_in, _chart_nas_net_out };
    _chart_axis_update(_chart_nas, LV_CHART_AXIS_PRIMARY_Y,   prim, 2, _nas_lblL, _nas_rangeL, 1);
    _chart_axis_update(_chart_nas, LV_CHART_AXIS_SECONDARY_Y, sec,  4, _nas_lblR, _nas_rangeR, 10);
    lv_chart_refresh(_chart_nas);
}
static void _refresh_chart_fb() {
    lv_chart_series_t* prim[] = { _chart_fb_down };
    lv_chart_series_t* sec[]  = { _chart_fb_up };
    _chart_axis_update(_chart_fb, LV_CHART_AXIS_PRIMARY_Y,   prim, 1, _fb_lblL, _fb_rangeL, 10);
    _chart_axis_update(_chart_fb, LV_CHART_AXIS_SECONDARY_Y, sec,  1, _fb_lblR, _fb_rangeR, 10);
    lv_chart_refresh(_chart_fb);
}

// Dépile un point en attente vers sa série. Le dépilage est inconditionnel
// (sur écran inactif LVGL bloque l'invalidation, c'est gratuit et l'historique
// reste continu) ; seul le marquage "dirty" dépend de l'écran affiché.
static void _chart_push_pending(bool& pending, lv_obj_t* chart, lv_chart_series_t* ser,
                                int value, lv_obj_t* screen, bool& dirty) {
    if (!pending) return;
    pending = false;
    if (ser) lv_chart_set_next_value(chart, ser, value);
    if (lv_scr_act() == screen) dirty = true;
}

// Alloue un buffer JSON en PSRAM, initialisé à "[]". nullptr si échec.
static char* _json_buf_alloc(size_t size) {
    char* p = (char*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) {
        log_line("[Display] PSRAM KO pour un buffer JSON (%u o)", (unsigned)size);
        return nullptr;
    }
    strcpy(p, "[]");
    return p;
}

// Recopie tronquée d'un payload. No-op si le buffer n'a pas pu être alloué.
static void _json_buf_set(char* dst, size_t size, const char* json) {
    if (!dst || !json) return;
    strncpy(dst, json, size - 1);
    dst[size - 1] = '\0';
}

// ---- API LOCALES ----

// --- LVGL / TFT_eSPI ---

static void _invalidate_spy_cb(lv_event_t* e) {
    // Lecture/écriture explicites : `_spy_remaining--` sur un volatile est
    // déprécié en C++20.
    uint8_t n = _spy_remaining;
    if (n == 0) return;
    _spy_remaining = n - 1;
    lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
    log_line("[LVGL] inval (%d,%d)-(%d,%d) %dx%d",
             (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2,
             (int)(a->x2 - a->x1 + 1), (int)(a->y2 - a->y1 + 1));
}

// TFT_eSPI attend du RGB565 big-endian ; LVGL 9 le produit déjà en
// LV_COLOR_FORMAT_RGB565_SWAPPED, on le passe directement.
static void _lv_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    if (_flush_count == 0) {
        _flush_x1 = area->x1; _flush_y1 = area->y1;
        _flush_x2 = area->x2; _flush_y2 = area->y2;
    } else {
        if (area->x1 < _flush_x1) _flush_x1 = area->x1;
        if (area->y1 < _flush_y1) _flush_y1 = area->y1;
        if (area->x2 > _flush_x2) _flush_x2 = area->x2;
        if (area->y2 > _flush_y2) _flush_y2 = area->y2;
    }
    _flush_count++;
    _flush_px += w * h;

    _tft.startWrite();
    _tft.setAddrWindow(area->x1, area->y1, w, h);
    _tft.pushPixels((uint16_t*)px_map, w * h);
    _tft.endWrite();

    lv_display_flush_ready(disp);
}

// --- Companion IA (ui_ScreenAI) ---

static void _ai_state_cb(AiState state) {
    _ai_last_activity_ms = millis();   // toute transition d'état réarme le compte à rebours
    lv_label_set_text(ui_LabelQuestion, ai_get_transcript());
    lv_label_set_text(ui_LabelAnswer, ai_get_answer());
    lv_label_set_text(ui_LabelRec,  state == AI_LISTENING ? "Stop" : "Rec");
    lv_label_set_text(ui_LabelPlay, state == AI_SPEAKING  ? "Stop" : "Play");
    ai_companion_set_state(state);
}

// --- Rattrapage écran — pousse le cache vers les widgets (SCREEN_LOADED) ---

static void _nas_apply_all() {
    lv_arc_set_value(ui_ArcNASCPU, _nas_state.cpu);
    lv_label_set_text_fmt(ui_LabelNASCPU, "%d%%", _nas_state.cpu);

    lv_arc_set_value(ui_ArcNASRAM, _nas_state.ram);
    lv_label_set_text_fmt(ui_LabelNASRAM, "%d%%", _nas_state.ram);

    lv_label_set_text_fmt(ui_LabelNASTemp, "Temp: %d °C", (int)_nas_state.temp);
    lv_label_set_text_fmt(ui_LabelNASNetRx, "Net In: %d kB/s", _nas_state.net_in);
    lv_label_set_text_fmt(ui_LabelNASNetTx, "Net Out: %d kB/s", _nas_state.net_out);
    lv_label_set_text_fmt(ui_LabelNASVol, "Vol: %d%%", _nas_state.vol_pct);
    lv_label_set_text_fmt(ui_LabelNASVolStatus, "Vol: %s", _nas_state.vol_status);
    lv_label_set_text_fmt(ui_LabelNASVolRead,  "Read: %d.%d MB/s",
                          FLOAT_INT(_nas_state.vol_read),  FLOAT_DEC(_nas_state.vol_read));
    lv_label_set_text_fmt(ui_LabelNASVolWrite, "Write: %d.%d MB/s",
                          FLOAT_INT(_nas_state.vol_write), FLOAT_DEC(_nas_state.vol_write));

    if (_chart_nas) _refresh_chart_nas();
}

static void _fb_apply_all() {
    lv_label_set_text_fmt(ui_LabelFbDown, "Rx: %d.%d Mb/s", FLOAT_INT(_fb_state.down), FLOAT_DEC(_fb_state.down));
    lv_label_set_text_fmt(ui_LabelFbUp,   "Tx: %d.%d Mb/s", FLOAT_INT(_fb_state.up),   FLOAT_DEC(_fb_state.up));
    lv_label_set_text_fmt(ui_LabelFbBwDown, "Max RX: %d Mb/s", _fb_state.bw_down);
    lv_label_set_text_fmt(ui_LabelFbBwUp,   "Max TX: %d Mb/s", _fb_state.bw_up);
    lv_label_set_text_fmt(ui_LabelFbState, "State: %s", _fb_state.state);
    lv_label_set_text_fmt(ui_LabelFbIPv4,  "IP: %s", _fb_state.ipv4);
    lv_label_set_text_fmt(ui_LabelFbDevicesActive, "Devices: %d", _fb_state.dev_active);
    lv_label_set_text_fmt(ui_LabelFbDevicesTotal,  "/ %d", _fb_state.dev_total);

    if (_chart_fb) _refresh_chart_fb();
}

// --- Table générique ---
// Le lv_table est persistant : détruit/recréé uniquement au changement de
// source (navigation), pas à chaque message MQTT — un simple rafraîchissement
// ne fait que réécrire les valeurs des cellules.

static void _table_setup_structure(TableSource source, const char* title,
                                   const char* col_headers[], const uint16_t col_widths[],
                                   uint8_t nb_cols, bool scroll_horizontal) {
    if (_title_label && lv_obj_is_valid(_title_label)) {
        lv_label_set_text(_title_label, title);
    } else {
        _title_label = lv_label_create(ui_ScreenTable);
        lv_obj_set_style_text_font(_title_label, &ui_font_Font12, LV_PART_MAIN); 
        lv_obj_set_x(_title_label, 5);
        lv_obj_set_y(_title_label, 8);
        lv_label_set_text(_title_label, title);
    }

    // Structure déjà en place pour cette source → rien à reconstruire
    if (_table_built_source == source && _lv_table && lv_obj_is_valid(_lv_table)) return;

    if (_lv_table) {
        lv_obj_delete(_lv_table);
        _lv_table = nullptr;
    }

    _lv_table = lv_table_create(ui_ScreenTable);
    lv_obj_set_x(_lv_table, 0);
    lv_obj_set_y(_lv_table, 35);
    lv_obj_set_width(_lv_table, 320);
    lv_obj_set_height(_lv_table, 170);   // laisse place au bouton Next à y=205
    // ui_font_Font10 (plage 0x20-0x17F) au lieu de montserrat_10 (ASCII seul) :
    // les accents des noms/titres venant du NAS/Freebox s'affichent enfin.
    lv_obj_set_style_text_font(_lv_table, &ui_font_Font10, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(_lv_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(_lv_table, 3, LV_PART_ITEMS);

    // Fond transparent = couleur de l'écran. Le « card » gris du thème LVGL par
    // défaut nuisait à la lisibilité. MAIN = le cadre, ITEMS = les cellules.
    lv_obj_set_style_bg_opa(_lv_table, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_lv_table, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_lv_table, LV_OPA_TRANSP, LV_PART_ITEMS);

    lv_table_set_column_count(_lv_table, nb_cols);

    for (uint8_t c = 0; c < nb_cols; c++) {
        lv_table_set_column_width(_lv_table, c, col_widths[c]);
        lv_table_set_cell_value(_lv_table, 0, c, col_headers[c]);
    }

    lv_obj_set_scroll_dir(_lv_table, scroll_horizontal ? (lv_dir_t)(LV_DIR_HOR | LV_DIR_VER) : LV_DIR_VER);

    _table_built_source = source;
}

// 1 ligne JSON = 1 ligne de table, dans l'ordre reçu.
static void _table_fill(const char* json, const char* keys[], uint8_t nb_cols) {
    if (!json) {   // buffer PSRAM non alloué
        lv_table_set_row_count(_lv_table, 2);
        lv_table_set_cell_value(_lv_table, 1, 0, "Pas de données");
        return;
    }

    JsonDocument doc;   // ArduinoJson 7 : allocation dynamique
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        lv_table_set_row_count(_lv_table, 2);
        lv_table_set_cell_value(_lv_table, 1, 0, "Erreur JSON");
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    uint16_t row = 1;
    char cell_buf[32];
    for (JsonObject obj : arr) {
        for (uint8_t c = 0; c < nb_cols; c++) {
            JsonVariant v = obj[keys[c]];
            if (v.isNull()) {
                lv_table_set_cell_value(_lv_table, row, c, "--");
            } else if (v.is<const char*>()) {
                lv_table_set_cell_value(_lv_table, row, c, v.as<const char*>());
            } else if (v.is<float>()) {
                float f = v.as<float>();
                int dec = (int)((f - (int)f) * 10 + 0.5f) % 10;
                if (dec == 0) snprintf(cell_buf, sizeof(cell_buf), "%d", (int)f);
                else          snprintf(cell_buf, sizeof(cell_buf), "%d.%d", (int)f, dec);
                lv_table_set_cell_value(_lv_table, row, c, cell_buf);
            } else if (v.is<int>()) {
                snprintf(cell_buf, sizeof(cell_buf), "%d", v.as<int>());
                lv_table_set_cell_value(_lv_table, row, c, cell_buf);
            } else {
                lv_table_set_cell_value(_lv_table, row, c, "--");
            }
        }
        row++;
        if (row > 30) break;
    }

    // Tronque les lignes d'un remplissage précédent plus grand (table persistante)
    lv_table_set_row_count(_lv_table, row);
}

static void _table_load(TableSource source) {
    _table_source = source;

    switch (source) {
        case TABLE_NAS_DISKS: {
            const char* hdrs[] = {"Drive", "Modèle", "Smart", "Status", "T°"};
            const uint16_t w[] = {62, 90, 64, 64, 40};
            const char* keys[] = {"name", "model", "smart_status", "status", "temp"};
            _table_setup_structure(TABLE_NAS_DISKS, "NAS - Disques", hdrs, w, 5, false);
            _table_fill(_json_nas_disks, keys, 5);
            break;
        }
        case TABLE_NAS_DOWNLOADS: {
            const char* hdrs[] = {"Titre", "UP", "DL", "Ratio", "Etat", "Size"};
            const uint16_t w[] = {120, 70, 70, 60, 100, 100};
            const char* keys[] = {"title", "speed_upload", "speed_download", "ratio", "status", "size_uploaded"};
            _table_setup_structure(TABLE_NAS_DOWNLOADS, "NAS - Downloads", hdrs, w, 6, true);
            _table_fill(_json_nas_downloads, keys, 6);
            break;
        }
        case TABLE_NAS_CONNECTIONS: {
            const char* hdrs[] = {"User", "IP", "Service"};
            const uint16_t w[] = {120, 110, 90};
            const char* keys[] = {"user", "from", "service"};
            _table_setup_structure(TABLE_NAS_CONNECTIONS, "NAS - Connexions", hdrs, w, 3, false);
            _table_fill(_json_nas_connections, keys, 3);
            break;
        }
        case TABLE_FBX_DEVICES: {
            // RX/TX = débits instantanés down/up, formatés avec unité par le bridge.
            const char* hdrs[] = {"Nom", "Link", "RX", "TX", "IP", "RxRate"};
            const uint16_t w[] = {105, 50, 80, 80, 105, 70};
            const char* keys[] = {"name", "type", "rx_rate", "tx_rate", "ip", "phy_rx_rate"};
            _table_setup_structure(TABLE_FBX_DEVICES, "Freebox - Devices", hdrs, w, 6, true);
            _table_fill(_json_fbx_devices, keys, 6);
            break;
        }
        case TABLE_FBX_ACTIVITY: {
            // Qui fait quoi : réutilise freebox/devices (enrichi d'un champ 'service' par le bridge).
            const char* hdrs[] = {"Appareil", "Service", "DL", "UP"};
            const uint16_t w[] = {110, 100, 55, 55};
            const char* keys[] = {"name", "service", "rx_rate", "tx_rate"};
            _table_setup_structure(TABLE_FBX_ACTIVITY, "Freebox - Activité Réseau", hdrs, w, 4, false);
            _table_fill(_json_fbx_devices, keys, 4);
            break;
        }
        default: break;
    }

    // Évite un lv_scr_load inutile : un rafraîchissement MQTT passe aussi ici.
    if (lv_scr_act() != ui_ScreenTable) lv_scr_load(ui_ScreenTable);
}

// ---- API PUBLIQUES ----

// --- Init / Loop ---

void display_init() {
    log_line("[Display] Init start");

    _tft.init();   // utilise le ILI9341_Init.h custom
    _tft.setRotation(1);
    _tft.fillScreen(TFT_BLACK);
    // Après init() pour ne pas conflicter avec le LEDC de TFT_eSPI.
    analogWrite(TFT_BL, map(DISPLAY_BRIGHTNESS_DEFAULT, 0, 100, 0, 255));

    log_line("[Display] TFT_eSPI OK");

    lv_init();

    // Buffer de dessin en RAM INTERNE. En PSRAM (2 × 40 lignes) le rendu était
    // bien plus lent : 115 ms pour un plein écran dont ~31 ms seulement de SPI,
    // le reste passé à écrire puis relire la PSRAM.
    _buf1 = (lv_color_t*)heap_caps_malloc(LV_BUF_BYTES,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Second buffer NON alloué pour l'instant : en mode PARTIAL il ne sert qu'à
    // préparer la bande suivante pendant le flush, or _lv_flush_cb est synchrone
    // (pushPixels, sans DMA) — il n'y a rien à recouvrir, et il doublerait le
    // coût en RAM interne. Gardé pour y revenir le jour où le flush passera en
    // DMA : il suffira de l'allouer comme _buf1.
    _buf2 = nullptr;

    if (!_buf1) {
        // Repli PSRAM plutôt qu'un écran mort : lent, mais fonctionnel.
        log_line("[Display] Buffer interne KO (%u o) → repli PSRAM, rendu lent",
                 (unsigned)LV_BUF_BYTES);
        _buf1 = (lv_color_t*)heap_caps_malloc(LV_BUF_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!_buf1) {
        log_line("[Display] FATAL: Buffer1 FAIL");
        return;
    }
    log_line("[Display] Buffer LVGL : %u o (%d lignes de %d px)",
             (unsigned)LV_BUF_BYTES, LV_BUF_LINES, SCREEN_WIDTH);

    // En PSRAM : simples zones de données relues à l'ouverture de l'écran Table,
    // jamais dans une boucle de rendu.
    _json_nas_disks       = _json_buf_alloc(JSON_NAS_DISKS_SIZE);
    _json_nas_downloads   = _json_buf_alloc(JSON_NAS_DOWNLOADS_SIZE);
    _json_nas_connections = _json_buf_alloc(JSON_NAS_CONNECTIONS_SIZE);
    _json_fbx_devices     = _json_buf_alloc(JSON_FBX_DEVICES_SIZE);

    _disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_default(_disp);
    lv_display_set_color_format(_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(_disp, _lv_flush_cb);
    lv_display_add_event_cb(_disp, _invalidate_spy_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_set_buffers(_disp, _buf1, _buf2, LV_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    log_line("[Display] LVGL OK");

    // Test visuel — valide la chaîne de flush avant de charger l'UI SquareLine
    lv_obj_t* label_test = lv_label_create(lv_scr_act());
    lv_label_set_text(label_test, "Initialisation...");
    lv_obj_center(label_test);

    unsigned long t0 = millis();
    while (millis() - t0 < 3000) {
        lv_tick_inc(5);
        lv_timer_handler();
        delay(5);
    }

    ui_init();
    display_show_home();

    log_line("[Display] UI OK");

    // --- Charts (créés ici, plus dans SquareLine) ---
    // Courbes ET labels d'échelle prennent la couleur de charte de leur métrique.
    // NAS gauche : CPU/RAM (%). Droite : 4 débits MB/s (R/W disque + réseau IN/OUT).
    // Le label d'échelle prend la couleur de la série primaire de l'axe (gauche
    // CPU, droite Read) ; les autres restent lisibles via leur readout coloré.
    // COL_RX/COL_TX = couleurs génériques réception/émission (readouts Net In/Out).
    _chart_nas = _chart_build(ui_ScreenNAS, COL_CPU, COL_READ, _nas_lblL, _nas_lblR);
    _chart_nas_cpu     = lv_chart_add_series(_chart_nas, COL_CPU,   LV_CHART_AXIS_PRIMARY_Y);
    _chart_nas_ram     = lv_chart_add_series(_chart_nas, COL_RAM,   LV_CHART_AXIS_PRIMARY_Y);
    _chart_nas_read    = lv_chart_add_series(_chart_nas, COL_READ,  LV_CHART_AXIS_SECONDARY_Y);
    _chart_nas_write   = lv_chart_add_series(_chart_nas, COL_WRITE, LV_CHART_AXIS_SECONDARY_Y);
    _chart_nas_net_in  = lv_chart_add_series(_chart_nas, COL_RX,    LV_CHART_AXIS_SECONDARY_Y);
    _chart_nas_net_out = lv_chart_add_series(_chart_nas, COL_TX,    LV_CHART_AXIS_SECONDARY_Y);

    _chart_fb = _chart_build(ui_ScreenFreebox, COL_RX, COL_TX, _fb_lblL, _fb_lblR);
    _chart_fb_down = lv_chart_add_series(_chart_fb, COL_RX, LV_CHART_AXIS_PRIMARY_Y);
    _chart_fb_up   = lv_chart_add_series(_chart_fb, COL_TX, LV_CHART_AXIS_SECONDARY_Y);

    // --- Clic audio sur les boutons de navigation ---
    auto _nav_click_cb = [](lv_event_t* e) { audio_click(); };
    lv_obj_add_event_cb(ui_BtnNAS,     _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnBackNAS, _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnNextNAS, _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnFreebox, _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnBackFbx, _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnNextFbx, _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnAI,      _nav_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_BtnBackAI,  _nav_click_cb, LV_EVENT_CLICKED, nullptr);

    // --- Navigation entre tables ---
    lv_obj_add_event_cb(ui_BtnBackTable, [](lv_event_t* e) {
        audio_click();
        if (_table_back_screen) lv_scr_load(_table_back_screen);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(ui_BtnNextTable, [](lv_event_t* e) {
        audio_click();
        _table_load((TableSource)((_table_source + 1) % TABLE_SOURCE_COUNT));
    }, LV_EVENT_CLICKED, nullptr);

    // --- Bouton SysInfo (écran de diagnostic, sysinfo_manager.cpp) ---
    lv_obj_add_event_cb(ui_BtnSysInfo, [](lv_event_t* e) {
        audio_ff6();
        display_show_sysinfo();
    }, LV_EVENT_CLICKED, nullptr);

    // --- Bouton Audio — test hardware micro/HP ---
    lv_obj_add_event_cb(ui_BtnAudio, [](lv_event_t* e) {
        audio_test_loopback();
    }, LV_EVENT_CLICKED, nullptr);

    // --- Bouton RecIA — démarre/arrête l'enregistrement ---
    lv_obj_add_event_cb(ui_BtnRecAI, [](lv_event_t* e) {
        audio_click();
        AiState s = ai_get_state();
        if (s == AI_LISTENING) ai_stop_listening();
        else if (s != AI_THINKING && s != AI_SPEAKING) {
            audio_wakeword_ack();   // même jingle que le déclenchement vocal
            ai_start_listening();
        }
    }, LV_EVENT_CLICKED, nullptr);

    // --- Bouton PlayIA — rejoue la réponse, ou stoppe si l'IA parle.
    // Le retour à AI_IDLE se fait dans display_loop() quand audio_is_playing
    // retombe à false. ---
    lv_obj_add_event_cb(ui_BtnPlayAI, [](lv_event_t* e) {
        audio_click();
        AiState s = ai_get_state();
        if (s == AI_SPEAKING) {
            audio_stop_playback();
            return;
        }
        if (s == AI_LISTENING) ai_cancel_listening();
        ai_replay_answer();
    }, LV_EVENT_CLICKED, nullptr);

    ai_set_state_callback(_ai_state_cb);

    // --- Sliders (LV_EVENT_ALL : ils traitent VALUE_CHANGED *et* RELEASED) ---
    lv_slider_set_range(ui_SliderLCD, 1, 100);   // 0 % = écran éteint
    lv_slider_set_value(ui_SliderLCD, DISPLAY_BRIGHTNESS_DEFAULT, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui_SliderLCD, [](lv_event_t* e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_VALUE_CHANGED) {
            int val = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
            log_line("[Slider] Backlight=%d", val);
            display_set_brightness(val);
        } else if (code == LV_EVENT_RELEASED) {
            audio_click();
        }
    }, LV_EVENT_ALL, nullptr);

    lv_slider_set_range(ui_SliderVOL, 0, 100);
    lv_slider_set_value(ui_SliderVOL, AUDIO_VOLUME_DEFAULT, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui_SliderVOL, [](lv_event_t* e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_VALUE_CHANGED) {
            int val = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
            log_line("[Slider] Volume=%d", val);
            audio_set_volume(val);
        } else if (code == LV_EVENT_RELEASED) {
            audio_click();
        }
    }, LV_EVENT_ALL, nullptr);

    // --- Rattrapage à l'affichage, sans attendre le prochain message MQTT ---
    lv_obj_add_event_cb(ui_ScreenNAS,     [](lv_event_t* e) { _nas_apply_all(); },      LV_EVENT_SCREEN_LOADED,   nullptr);
    lv_obj_add_event_cb(ui_ScreenFreebox, [](lv_event_t* e) { _fb_apply_all(); },       LV_EVENT_SCREEN_LOADED,   nullptr);
    lv_obj_add_event_cb(ui_ScreenAI,      [](lv_event_t* e) { ai_companion_resume(); }, LV_EVENT_SCREEN_LOADED,   nullptr);
    lv_obj_add_event_cb(ui_ScreenAI,      [](lv_event_t* e) { ai_companion_pause(); },  LV_EVENT_SCREEN_UNLOADED, nullptr);

    log_line("[Display] Init DONE");
}

void display_loop() {
    if (_display_paused) return;   // pas d'accès SPI concurrent (cf. display_pause)

    unsigned long now = millis();
    if (now - _last_lv_tick >= LV_TICK_PERIOD_MS) {
        lv_tick_inc(now - _last_lv_tick);
        _last_lv_tick = now;
    }
    // Forcer un redraw LVGL (après un resume)
    if (_force_redraw) {
        _force_redraw = false;
        lv_obj_invalidate(lv_scr_act());
    }
    // Retour auto à l'écran précédent après inactivité sur l'écran IA (entrée
    // vocale/MQTT). Désarmé dès qu'on quitte l'IA autrement (bouton Back, nav).
    // Jamais pendant une conversation : garde sur AI_IDLE. Inactivité = le plus
    // récent d'un changement d'état IA ou d'un toucher écran.
    if (_ai_return_armed) {
        if (lv_scr_act() != ui_ScreenAI) {
            _ai_return_armed = false;
        } else if (ai_get_state() == AI_IDLE) {
            uint32_t idle = now - _ai_last_activity_ms;
            uint32_t touch = lv_display_get_inactive_time(NULL);
            if (touch < idle) idle = touch;
            if (idle >= AI_SCREEN_RETURN_MS) {
                _ai_return_armed = false;
                if (_ai_back_screen && _ai_back_screen != ui_ScreenAI)
                    lv_scr_load(_ai_back_screen);
            }
        }
    }

    // Dépilage des points en attente PUIS refresh, dans une seule frame.
    if (now - _last_chart_refresh >= CHART_REFRESH_MIN_INTERVAL_MS) {
        _last_chart_refresh = now;

        _chart_push_pending(_nas_chart_pending.has_cpu,   _chart_nas, _chart_nas_cpu,
                            _nas_chart_pending.cpu, ui_ScreenNAS, _nas_chart_dirty);
        _chart_push_pending(_nas_chart_pending.has_ram,   _chart_nas, _chart_nas_ram,
                            _nas_chart_pending.ram, ui_ScreenNAS, _nas_chart_dirty);
        _chart_push_pending(_nas_chart_pending.has_read,  _chart_nas, _chart_nas_read,
                            (int)(_nas_chart_pending.read * 10), ui_ScreenNAS, _nas_chart_dirty);
        _chart_push_pending(_nas_chart_pending.has_write, _chart_nas, _chart_nas_write,
                            (int)(_nas_chart_pending.write * 10), ui_ScreenNAS, _nas_chart_dirty);
        _chart_push_pending(_nas_chart_pending.has_net_in,  _chart_nas, _chart_nas_net_in,
                            _nas_chart_pending.net_in / 100, ui_ScreenNAS, _nas_chart_dirty);   // kB/s -> MB/s×10
        _chart_push_pending(_nas_chart_pending.has_net_out, _chart_nas, _chart_nas_net_out,
                            _nas_chart_pending.net_out / 100, ui_ScreenNAS, _nas_chart_dirty);
        _chart_push_pending(_fb_chart_pending.has_down,   _chart_fb, _chart_fb_down,
                            (int)(_fb_chart_pending.down * 10), ui_ScreenFreebox, _fb_chart_dirty);
        _chart_push_pending(_fb_chart_pending.has_up,     _chart_fb, _chart_fb_up,
                            (int)(_fb_chart_pending.up * 10), ui_ScreenFreebox, _fb_chart_dirty);

        if (_nas_chart_dirty && lv_scr_act() == ui_ScreenNAS) { _refresh_chart_nas(); _nas_chart_dirty = false; }
        if (_fb_chart_dirty && lv_scr_act() == ui_ScreenFreebox) { _refresh_chart_fb(); _fb_chart_dirty = false; }
    }

    _flush_count = 0;
    _flush_px    = 0;

    unsigned long t0 = millis();
    _in_lvgl = true;
    lv_timer_handler();
    _in_lvgl = false;
    unsigned long dt = millis() - t0;

    // Format compact : LOG_LINE_LEN vaut 100 OCTETS, dont 10 d'horodatage.
    if (dt > 100) {
        uint32_t pct = (_flush_px * 100) / ((uint32_t)SCREEN_WIDTH * SCREEN_HEIGHT);
        log_line("[LVGL] slow %lums — %u flush, %lu px (%lu%%), (%d,%d)-(%d,%d)",
                 dt, (unsigned)_flush_count, (unsigned long)_flush_px, (unsigned long)pct,
                 (int)_flush_x1, (int)_flush_y1, (int)_flush_x2, (int)_flush_y2);
    }

    bool speaking_now = audio_is_playing;
    if (_ai_was_speaking && !speaking_now) ai_notify_speaking_done();
    _ai_was_speaking = speaking_now;
}

// LVGL et OTA partagent le même bus SPI sans mutex (SUPPORT_TRANSACTIONS
// désactivé) : une écriture concurrente corrompt la liaison avec l'écran.
//
// ⚠️ Poser le drapeau NE SUFFIT PAS, et c'est ce qui faisait paniquer un OTA sur
// trois : il n'empêche que la frame SUIVANTE. Au moment où ota_task (cœur 0)
// appelle ceci, loopTask (cœur 1) peut être DANS lv_timer_handler(), donc dans
// _lv_flush_cb entre startWrite() et endWrite(). L'appelant enchaînait alors
// sur display_show_ota_progress() -> _tft.fillScreen(), ouvrant une seconde
// transaction SPI sur le même _tft — abort du pilote, reset "panic".
// On attend donc la sortie effective de LVGL avant de rendre la main.
void display_pause() {
    _display_paused = true;

    // Pas de deadlock : le seul appelant est ota_task (cœur 0, priorité 2),
    // loopTask tourne sur l'autre cœur et progresse toujours. Le timeout ne
    // couvre qu'une frame lente (270 ms mesurés au pire) avec large marge ;
    // l'atteindre signalerait un blocage réel, d'où le log.
    unsigned long t0 = millis();
    while (_in_lvgl && millis() - t0 < DISPLAY_PAUSE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (_in_lvgl) log_line("[Display] pause : LVGL toujours actif apres %lu ms",
                           (unsigned long)DISPLAY_PAUSE_TIMEOUT_MS);
}

void display_resume() {
    _display_paused = false;
    _last_lv_tick = millis();   // évite un lv_tick_inc() géant en sortie de pause
    _force_redraw = true;       // on force un rendu LVGL
}

// --- Navigation ---

void display_show_home()    { lv_scr_load(ui_ScreenHome); }
void display_show_nas()     { lv_scr_load(ui_ScreenNAS); }
void display_show_freebox() { lv_scr_load(ui_ScreenFreebox); }
void display_show_ai() { lv_scr_load(ui_ScreenAI); }   // MQTT / tactile : pas de retour auto

// Entrée vocale uniquement : arme le retour auto vers l'écran précédent.
void display_show_ai_auto() {
    lv_obj_t* cur = lv_scr_act();
    if (cur != ui_ScreenAI) _ai_back_screen = cur;
    _ai_last_activity_ms = millis();
    _ai_return_armed = true;
    lv_scr_load(ui_ScreenAI);
}

// --- Table générique (callbacks SquareLine) ---
extern "C" {

void display_show_TABLE_NAS_DISKS(lv_event_t* e) {
    _table_back_screen = ui_ScreenNAS;
    _table_load(TABLE_NAS_DISKS);
}

void display_show_TABLE_NAS_DOWNLOADS(lv_event_t* e) {
    _table_back_screen = ui_ScreenNAS;
    _table_load(TABLE_NAS_DOWNLOADS);
}

void display_show_TABLE_NAS_CONNECTIONS(lv_event_t* e) {
    _table_back_screen = ui_ScreenNAS;
    _table_load(TABLE_NAS_CONNECTIONS);
}

void display_show_TABLE_FBX_DEVICES(lv_event_t* e) {
    _table_back_screen = ui_ScreenFreebox;
    _table_load(TABLE_FBX_DEVICES);
}

void display_show_TABLE_FBX_ACTIVITY(lv_event_t* e) {
    _table_back_screen = ui_ScreenFreebox;
    _table_load(TABLE_FBX_ACTIVITY);
}

} // extern "C"

// --- NAS ---
//
// Pattern commun : le point de chart part dans _nas_chart_pending (jamais poussé
// directement), et le label n'est réécrit QUE si la valeur change —
// lv_label_set_text invalide même à texte identique, et sur un NAS au repos
// cpu/ram/temp ne bougent pas d'une publication à l'autre. L'écran reste juste
// à l'entrée grâce à _nas_apply_all() sur SCREEN_LOADED.

void display_update_nas_cpu(int percent) {
    bool changed = (percent != _nas_state.cpu);
    _nas_state.cpu = percent;
    _nas_chart_pending.cpu = percent;
    _nas_chart_pending.has_cpu = true;

    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_arc_set_value(ui_ArcNASCPU, percent);
    lv_label_set_text_fmt(ui_LabelNASCPU, "%d%%", percent);
}

void display_update_nas_ram(int percent) {
    bool changed = (percent != _nas_state.ram);
    _nas_state.ram = percent;
    _nas_chart_pending.ram = percent;
    _nas_chart_pending.has_ram = true;

    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_arc_set_value(ui_ArcNASRAM, percent);
    lv_label_set_text_fmt(ui_LabelNASRAM, "%d%%", percent);
}

void display_update_nas_temp(float celsius) {
    bool changed = ((int)celsius != (int)_nas_state.temp);   // granularité affichée
    _nas_state.temp = celsius;
    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASTemp, "Temp: %d °C", (int)celsius);
}

void display_update_nas_net_in(int kbps) {
    bool changed = (kbps != _nas_state.net_in);
    _nas_state.net_in = kbps;
    _nas_chart_pending.net_in = kbps;         // point déposé quel que soit l'écran actif
    _nas_chart_pending.has_net_in = true;
    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASNetRx, "Net In: %d kB/s", kbps);
}

void display_update_nas_net_out(int kbps) {
    bool changed = (kbps != _nas_state.net_out);
    _nas_state.net_out = kbps;
    _nas_chart_pending.net_out = kbps;
    _nas_chart_pending.has_net_out = true;
    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASNetTx, "Net Out: %d kB/s", kbps);
}

void display_update_nas_vol_pct(int percent) {
    bool changed = (percent != _nas_state.vol_pct);
    _nas_state.vol_pct = percent;
    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASVol, "Vol: %d%%", percent);
}

void display_update_nas_vol_status(const char* status) {
    bool changed = (strncmp(status, _nas_state.vol_status, sizeof(_nas_state.vol_status)) != 0);
    strncpy(_nas_state.vol_status, status, sizeof(_nas_state.vol_status) - 1);
    _nas_state.vol_status[sizeof(_nas_state.vol_status) - 1] = '\0';
    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASVolStatus, "Vol: %s", status);
}

void display_update_nas_vol_read(float mbs) {
    bool changed = (mbs != _nas_state.vol_read);
    _nas_state.vol_read = mbs;
    _nas_chart_pending.read = mbs;
    _nas_chart_pending.has_read = true;

    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASVolRead, "Read: %d.%d MB/s", FLOAT_INT(mbs), FLOAT_DEC(mbs));
}

void display_update_nas_vol_write(float mbs) {
    bool changed = (mbs != _nas_state.vol_write);
    _nas_state.vol_write = mbs;
    _nas_chart_pending.write = mbs;
    _nas_chart_pending.has_write = true;

    if (!changed || lv_scr_act() != ui_ScreenNAS) return;
    lv_label_set_text_fmt(ui_LabelNASVolWrite, "Write: %d.%d MB/s", FLOAT_INT(mbs), FLOAT_DEC(mbs));
}

void display_update_nas_disks(const char* json) {
    _json_buf_set(_json_nas_disks, JSON_NAS_DISKS_SIZE, json);
    if (lv_scr_act() == ui_ScreenTable && _table_source == TABLE_NAS_DISKS)
        _table_load(TABLE_NAS_DISKS);
}

void display_update_nas_downloads(const char* json) {
    _json_buf_set(_json_nas_downloads, JSON_NAS_DOWNLOADS_SIZE, json);
    if (lv_scr_act() == ui_ScreenTable && _table_source == TABLE_NAS_DOWNLOADS)
        _table_load(TABLE_NAS_DOWNLOADS);
}

void display_update_nas_connections(const char* json) {
    _json_buf_set(_json_nas_connections, JSON_NAS_CONNECTIONS_SIZE, json);
    if (lv_scr_act() == ui_ScreenTable && _table_source == TABLE_NAS_CONNECTIONS)
        _table_load(TABLE_NAS_CONNECTIONS);
}

// --- Freebox (même pattern que les setters NAS) ---

void display_update_fb_down(float mbps) {
    bool changed = (mbps != _fb_state.down);
    _fb_state.down = mbps;
    _fb_chart_pending.down = mbps;
    _fb_chart_pending.has_down = true;

    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbDown, "Rx: %d.%d Mb/s", FLOAT_INT(mbps), FLOAT_DEC(mbps));
}

void display_update_fb_up(float mbps) {
    bool changed = (mbps != _fb_state.up);
    _fb_state.up = mbps;
    _fb_chart_pending.up = mbps;
    _fb_chart_pending.has_up = true;

    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbUp, "Tx: %d.%d Mb/s", FLOAT_INT(mbps), FLOAT_DEC(mbps));
}

void display_update_fb_bw_down(int mbps) {
    bool changed = (mbps != _fb_state.bw_down);
    _fb_state.bw_down = mbps;
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbBwDown, "Max RX: %d Mb/s", mbps);
}

void display_update_fb_bw_up(int mbps) {
    bool changed = (mbps != _fb_state.bw_up);
    _fb_state.bw_up = mbps;
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbBwUp, "Max TX: %d Mb/s", mbps);
}

void display_update_fb_state(const char* state) {
    bool changed = (strncmp(state, _fb_state.state, sizeof(_fb_state.state)) != 0);
    strncpy(_fb_state.state, state, sizeof(_fb_state.state) - 1);
    _fb_state.state[sizeof(_fb_state.state) - 1] = '\0';
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbState, "State: %s", state);
}

void display_update_fb_ipv4(const char* ip) {
    bool changed = (strncmp(ip, _fb_state.ipv4, sizeof(_fb_state.ipv4)) != 0);
    strncpy(_fb_state.ipv4, ip, sizeof(_fb_state.ipv4) - 1);
    _fb_state.ipv4[sizeof(_fb_state.ipv4) - 1] = '\0';
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbIPv4, "IP: %s", ip);
}

void display_update_fb_devices_active(int count) {
    bool changed = (count != _fb_state.dev_active);
    _fb_state.dev_active = count;
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbDevicesActive, "Devices: %d", count);
}

void display_update_fb_devices_total(int count) {
    bool changed = (count != _fb_state.dev_total);
    _fb_state.dev_total = count;
    if (!changed || lv_scr_act() != ui_ScreenFreebox) return;
    lv_label_set_text_fmt(ui_LabelFbDevicesTotal, "/ %d", count);
}

void display_update_fb_devices(const char* json) {
    _json_buf_set(_json_fbx_devices, JSON_FBX_DEVICES_SIZE, json);
    // Les deux tables lisent freebox/devices : rafraîchir celle qui est affichée.
    if (lv_scr_act() == ui_ScreenTable &&
        (_table_source == TABLE_FBX_DEVICES || _table_source == TABLE_FBX_ACTIVITY))
        _table_load(_table_source);
}

// --- Utils ---

// Déclarée dans un en-tête PRIVÉ de LVGL (lv_obj_draw_private.h) que lvgl.h
// n'inclut pas : on redéclare le prototype plutôt que de parier sur le chemin
// d'inclusion retenu par PlatformIO. Sert au dump d'arbre uniquement.
extern "C" int32_t lv_obj_get_ext_draw_size(const lv_obj_t* obj);

static const char* _obj_class_name(lv_obj_t* o) {
    const lv_obj_class_t* c = lv_obj_get_class(o);
    if (c == &lv_label_class)  return "label";
    if (c == &lv_arc_class)    return "arc";
    if (c == &lv_chart_class)  return "chart";
    if (c == &lv_scale_class)  return "scale";
    if (c == &lv_button_class) return "btn";
    if (c == &lv_image_class)  return "img";
    if (c == &lv_slider_class) return "slider";
    if (c == &lv_canvas_class) return "canvas";
    return "obj";
}

static void _tree_rec(lv_obj_t* o, int depth) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    char txt[16] = "";
    if (lv_obj_get_class(o) == &lv_label_class)
        snprintf(txt, sizeof(txt), " '%.10s'", lv_label_get_text(o));
    // ext_draw_size : marge que LVGL ajoute AUTOUR de l'objet à l'invalidation,
    // pour couvrir ce qui déborde. C'est elle qui transforme un chart de 280x95
    // en repeint de 320x179.
    int32_t ext = lv_obj_get_ext_draw_size(o);
    log_line("[TREE]%*s%s%s (%d,%d)-(%d,%d) ext%d", depth * 2, "", _obj_class_name(o), txt,
             (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, (int)ext);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) _tree_rec(lv_obj_get_child(o, i), depth + 1);
}

void display_dump_tree() {
    lv_obj_t* scr = lv_scr_act();
    const char* name = scr == ui_ScreenNAS     ? "NAS"
                     : scr == ui_ScreenFreebox ? "Freebox"
                     : scr == ui_ScreenAI      ? "AI"
                     : scr == ui_ScreenHome    ? "Home"
                     : scr == ui_ScreenTable   ? "Table" : "?";
    log_line("[TREE] === ecran actif : %s (%u enfants) ===", name,
             (unsigned)lv_obj_get_child_count(scr));
    _tree_rec(scr, 0);

    // Seuls objets hors écran actif que lv_obj_invalidate laisse atteindre le
    // display : si un rectangle de l'espion ne correspond à rien, il vient d'ici.
    if (lv_obj_get_child_count(lv_layer_top()) > 0) {
        log_line("[TREE] === layer_top ===");
        _tree_rec(lv_layer_top(), 0);
    }
    if (lv_obj_get_child_count(lv_layer_sys()) > 0) {
        log_line("[TREE] === layer_sys ===");
        _tree_rec(lv_layer_sys(), 0);
    }
}

void display_spy_invalidations(uint8_t count) {
    _spy_remaining = count;
    log_line("[LVGL] Espion arme — les %u prochaines invalidations seront tracees", (unsigned)count);
}

// Affichage en TFT_eSPI direct (hors LVGL) : reste visible même si LVGL est
// bloqué pendant le transfert. Appelé depuis le callback OTA (tâche WiFi).
void display_show_ota_progress(int percent) {
    // ⚠️ Le décor ne se dessine qu'UNE fois, sur le seul _ota_screen_init.
    // Le drapeau est remis à false aux deux sorties
    // 100 % plus bas, et display_show_ota_error : un OTA suivant
    // repart bien d'un écran propre sans cette condition.
    if (!_ota_screen_init) {
        _ota_screen_init = true;
        _tft.fillScreen(TFT_BLACK);
        _tft.setTextColor(TFT_CYAN, TFT_BLACK);
        _tft.setTextSize(2);
        const char* title = "MISE A JOUR OTA";
        _tft.setCursor((SCREEN_WIDTH - strlen(title) * 12) / 2, 60);
        _tft.print(title);
        _tft.setTextSize(1);
        _tft.setTextColor(TFT_WHITE, TFT_BLACK);
        _tft.setCursor((SCREEN_WIDTH - 18 * 6) / 2, 90);
        _tft.print("Ne pas eteindre !");
        _tft.drawRect(20, 120, SCREEN_WIDTH - 40, 20, TFT_WHITE);
    }

    int barW = (int)((SCREEN_WIDTH - 42) * constrain(percent, 0, 100) / 100.0f);
    _tft.fillRect(21, 121, barW, 18, TFT_GREEN);

    char buf[8];
    snprintf(buf, sizeof(buf), " %3d%% ", percent);
    _tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    _tft.setTextSize(2);
    _tft.setCursor((SCREEN_WIDTH - 6 * 12) / 2, 150);
    _tft.print(buf);

    if (percent >= 100) {
        _ota_screen_init = false;
        _tft.setTextColor(TFT_GREEN, TFT_BLACK);
        _tft.setTextSize(1);
        _tft.setCursor((SCREEN_WIDTH - 22 * 6) / 2, 180);
        _tft.print("Redemarrage en cours...");
    }
}

// Échec OTA, en TFT direct comme la barre de progression.
void display_show_ota_error(const char* reason) {
    _ota_screen_init = false;   // la prochaine progression repartira d'un écran propre

    _tft.fillScreen(TFT_BLACK);
    _tft.setTextColor(TFT_RED, TFT_BLACK);
    _tft.setTextSize(2);
    const char* title = "ECHEC OTA";
    _tft.setCursor((SCREEN_WIDTH - strlen(title) * 12) / 2, 80);
    _tft.print(title);

    _tft.setTextSize(1);
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.setCursor((SCREEN_WIDTH - strlen(reason) * 6) / 2, 120);
    _tft.print(reason);

    _tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    const char* hint1 = "firmware precedent conserve";
    _tft.setCursor((SCREEN_WIDTH - strlen(hint1) * 6) / 2, 145);
    _tft.print(hint1);

    _tft.setTextColor(TFT_WHITE, TFT_RED);
    const char* hint2 = "FAITES UN NOUVEL ESSAI OU REDEMARREZ";
    _tft.setCursor((SCREEN_WIDTH - strlen(hint2) * 6) / 2, 180);
    _tft.print(hint2);

}

void display_set_brightness(int percent) {
    int duty = map(constrain(percent, 0, 100), 0, 100, 0, 255);
    analogWrite(TFT_BL, duty);
    lv_slider_set_value(ui_SliderLCD, constrain(percent, 0, 100), LV_ANIM_OFF);
}

void display_sync_volume_slider(int percent) {
    lv_slider_set_value(ui_SliderVOL, constrain(percent, 0, 100), LV_ANIM_OFF);
}
