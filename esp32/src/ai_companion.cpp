// ============================================================
// AI_COMPANION.CPP — avatar 120x120 sur ui_ImageCompanion (ui_ScreenAI)
//
// Frames chargées depuis LittleFS (/companion/*.bin) en PSRAM au démarrage,
// allocation unique réutilisée pour toute la durée de vie.
//
// Format des .bin : PAS de header — les octets bruts RGB565A8 (plan couleur
// 120*120*2, puis plan alpha 120*120*1). Le lv_image_dsc_t est reconstruit ici.
//
// idle    : 4 frames — respiration douce, tourne en continu
// listen  : 6 frames — pulse d'anneau
// think   : 6 frames — rotation des 3 points
// speak   : 4 frames — bouche fermée -> mi -> ouverte -> mi
// error   : 1 frame  — état ponctuel, pas d'animation
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <lvgl.h>
#include <esp_heap_caps.h>

// ---- RESSOURCES LOCALES ----
#include "ai_companion.h"
#include "../squareline/ui/ui.h"
#include "littlefs_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
// Géométrie dans ai_companion.h : _si_alloc en tire l'empreinte PSRAM.
#define FRAME_W        COMPANION_FRAME_W
#define FRAME_H        COMPANION_FRAME_H
#define FRAME_COLOR_SZ (FRAME_W * FRAME_H * 2)   // plan RGB565
#define FRAME_ALPHA_SZ (FRAME_W * FRAME_H * 1)   // plan A8
#define FRAME_TOTAL_SZ (FRAME_COLOR_SZ + FRAME_ALPHA_SZ)  // 43200 octets

static const char* const _frame_names[] = {
    "idle_1", "idle_2", "idle_3", "idle_4",
    "listen_1", "listen_2", "listen_3", "listen_4", "listen_5", "listen_6",
    "think_1", "think_2", "think_3", "think_4", "think_5", "think_6",
    "speak_1", "speak_2", "speak_3", "speak_4",
    "error",
};
#define FRAME_COUNT (sizeof(_frame_names) / sizeof(_frame_names[0]))  // 21

static_assert(FRAME_COUNT == COMPANION_FRAME_COUNT,
              "COMPANION_FRAME_COUNT (ai_companion.h) doit suivre _frame_names[]");
static_assert(FRAME_TOTAL_SZ == COMPANION_FRAME_SZ,
              "COMPANION_FRAME_SZ (ai_companion.h) doit suivre les plans RGB565+A8");

static lv_image_dsc_t _dsc[FRAME_COUNT];   // headers, remplis au chargement
static bool            _loaded = false;

// Index de départ de chaque groupe dans _frame_names/_dsc
#define IDX_IDLE    0
#define IDX_LISTEN  4
#define IDX_THINK   10
#define IDX_SPEAK   16
#define IDX_ERROR   20

static const lv_image_dsc_t* _frames_idle[4];
static const lv_image_dsc_t* _frames_listen[6];
static const lv_image_dsc_t* _frames_think[6];
static const lv_image_dsc_t* _frames_speak[4];

// Animation — timer + lv_image_set_src (objet Image standard SquareLine)
static lv_timer_t*             _anim_timer     = nullptr;
static const lv_image_dsc_t**  _current_frames = nullptr;
static uint8_t                 _current_count  = 0;
static uint8_t                 _current_idx    = 0;

// Dernier état demandé, pour reprendre exactement là où on était
static AiState _last_state = AI_IDLE;

// Avant-déclaration — définie en API LOCALES, appelée depuis _start_anim()
static void _anim_timer_cb(lv_timer_t* t);

// ---- HELPERS ----

// Échec silencieux + log si un fichier manque : l'avatar reste figé plutôt
// que de planter l'écran Companion.
static bool _load_frame(uint8_t idx) {
    char path[48];
    snprintf(path, sizeof(path), "/companion/%s.bin", _frame_names[idx]);

    fs::File f = littlefs_open(path, "r");
    if (!f) {
        log_line("[Companion] Fichier manquant : %s", path);
        return false;
    }
    if (f.size() != FRAME_TOTAL_SZ) {
        log_line("[Companion] Taille inattendue pour %s : %u (attendue %u)",
                 path, (unsigned)f.size(), (unsigned)FRAME_TOTAL_SZ);
        f.close();
        return false;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(FRAME_TOTAL_SZ, MALLOC_CAP_SPIRAM);
    if (!buf) {
        log_line("[Companion] Alloc PSRAM échouée pour %s", path);
        f.close();
        return false;
    }

    size_t read = f.read(buf, FRAME_TOTAL_SZ);
    f.close();
    if (read != FRAME_TOTAL_SZ) {
        log_line("[Companion] Lecture incomplète pour %s : %u/%u",
                 path, (unsigned)read, (unsigned)FRAME_TOTAL_SZ);
        heap_caps_free(buf);
        return false;
    }

    _dsc[idx].header.magic = LV_IMAGE_HEADER_MAGIC;
    _dsc[idx].header.cf    = LV_COLOR_FORMAT_RGB565A8;
    _dsc[idx].header.w     = FRAME_W;
    _dsc[idx].header.h     = FRAME_H;
    _dsc[idx].data_size    = FRAME_TOTAL_SZ;
    _dsc[idx].data         = buf;

    return true;
}

static void _stop_anim() {
    if (_anim_timer) {
        lv_timer_del(_anim_timer);
        _anim_timer = nullptr;
    }
    _current_frames = nullptr;
}

static void _start_anim(const lv_image_dsc_t** frames, uint8_t count, uint32_t frame_ms) {
    if (_current_frames == frames) return;   // déjà sur cette anim, ne pas relancer

    _stop_anim();
    _current_frames = frames;
    _current_count  = count;
    _current_idx    = 0;
    lv_image_set_src(ui_ImageCompanion, frames[0]);
    _anim_timer = lv_timer_create(_anim_timer_cb, frame_ms, nullptr);
}

// ---- SÉQUENCE D'INITIALISATION ----

static bool _load_all_frames() {
    bool all_ok = true;
    for (uint8_t i = 0; i < FRAME_COUNT; i++) {
        if (!_load_frame(i)) all_ok = false;
    }
    for (uint8_t i = 0; i < 4; i++) _frames_idle[i]   = &_dsc[IDX_IDLE + i];
    for (uint8_t i = 0; i < 6; i++) _frames_listen[i] = &_dsc[IDX_LISTEN + i];
    for (uint8_t i = 0; i < 6; i++) _frames_think[i]  = &_dsc[IDX_THINK + i];
    for (uint8_t i = 0; i < 4; i++) _frames_speak[i]  = &_dsc[IDX_SPEAK + i];
    return all_ok;
}

// ---- API LOCALES ----

static void _anim_timer_cb(lv_timer_t* t) {
    if (!_current_frames || _current_count == 0) return;
    _current_idx = (_current_idx + 1) % _current_count;
    lv_image_set_src(ui_ImageCompanion, _current_frames[_current_idx]);
}

// ---- API PUBLIQUES ----

void ai_companion_init() {
    _loaded = _load_all_frames();
    if (!_loaded) {
        log_line("[Companion] Chargement incomplet — vérifie 'pio run -t uploadfs'");
        return;   // évite de pointer lv_image_set_src sur un buffer nullptr
    }
    lv_image_set_src(ui_ImageCompanion, _frames_idle[0]);   // avatar figé, pas vide

    log_line("[Companion] Avatar chargé en PSRAM depuis LittleFS OK");
}

void ai_companion_set_state(AiState state) {
    if (!_loaded) return;   // pas de frames en mémoire, rien à afficher
    _last_state = state;
    switch (state) {
        case AI_IDLE:
            _start_anim(_frames_idle, 4, 120);
            break;
        case AI_LISTENING:
            _start_anim(_frames_listen, 6, 90);
            break;
        case AI_THINKING:
            _start_anim(_frames_think, 6, 130);
            break;
        case AI_SPEAKING:
            _start_anim(_frames_speak, 4, 110);
            break;
        case AI_ERROR:
            _stop_anim();
            lv_image_set_src(ui_ImageCompanion, &_dsc[IDX_ERROR]);
            break;
    }
}

void ai_companion_pause() {
    _stop_anim();   // libère le timer tant que l'écran Companion n'est pas affiché
}

void ai_companion_resume() {
    ai_companion_set_state(_last_state);
}
