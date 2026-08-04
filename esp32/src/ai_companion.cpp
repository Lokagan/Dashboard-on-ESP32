// ============================================================
// AI_COMPANION.CPP — avatar 120x120 sur ui_ImageCompanion (ui_ScreenAI)
//
// Frames chargées depuis LittleFS (/companion/*.bin) en PSRAM au démarrage,
// allocation unique réutilisée pour toute la durée de vie.
//
// Format des .bin : PAS de header — les octets bruts RGB565 (120*120*2), sans
// plan alpha. Le lv_image_dsc_t est reconstruit ici.
//
// idle     : 8 frames  — respiration et clignements, tourne en continu
// listen   : 6 frames  — attention, regard soutenu
// think    : 8 frames  — regard qui cherche
// speak    : 12 frames — cycle de bouche
// response : 4 frames  — sourire, joué EN ONE-SHOT pendant l'idle
// error    : 4 frames  — glitch
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

// ---- RESSOURCES LOCALES ----
#include "ai_companion.h"
#include "../squareline/ui/ui.h"
#include "littlefs_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
// Géométrie dans ai_companion.h : _si_alloc en tire l'empreinte PSRAM.
#define FRAME_W        COMPANION_FRAME_W
#define FRAME_H        COMPANION_FRAME_H
#define FRAME_TOTAL_SZ (FRAME_W * FRAME_H * 2)   // plan RGB565, 28800 octets

static const char* const _frame_names[] = {
    "idle_1", "idle_2", "idle_3", "idle_4", "idle_5", "idle_6", "idle_7", "idle_8",
    "listen_1", "listen_2", "listen_3", "listen_4", "listen_5", "listen_6",
    "think_1", "think_2", "think_3", "think_4", "think_5", "think_6", "think_7", "think_8",
    "speak_1", "speak_2", "speak_3", "speak_4", "speak_5", "speak_6",
    "speak_7", "speak_8", "speak_9", "speak_10", "speak_11", "speak_12",
    "response_1", "response_2", "response_3", "response_4",
    "error_1", "error_2", "error_3", "error_4",
};
#define FRAME_COUNT (sizeof(_frame_names) / sizeof(_frame_names[0]))  // 42

static_assert(FRAME_COUNT == COMPANION_FRAME_COUNT,
              "COMPANION_FRAME_COUNT (ai_companion.h) doit suivre _frame_names[]");
static_assert(FRAME_TOTAL_SZ == COMPANION_FRAME_SZ,
              "COMPANION_FRAME_SZ (ai_companion.h) doit suivre le plan RGB565");

static lv_image_dsc_t _dsc[FRAME_COUNT];   // headers, remplis au chargement
static bool            _loaded = false;

// Index de départ et effectif de chaque groupe dans _frame_names/_dsc
#define IDX_IDLE     0
#define IDX_LISTEN   8
#define IDX_THINK    14
#define IDX_SPEAK    22
#define IDX_RESPONSE 34
#define IDX_ERROR    38

#define N_IDLE     8
#define N_LISTEN   6
#define N_THINK    8
#define N_SPEAK    12
#define N_RESPONSE 4
#define N_ERROR    4

// Cadences, en ms par frame
#define MS_IDLE     130
#define MS_LISTEN   90
#define MS_THINK    110
#define MS_SPEAK    80
#define MS_RESPONSE 110
#define MS_ERROR    90

// Fenêtre de tirage du one-shot « response » pendant l'idle
#define RESPONSE_MIN_MS 8000
#define RESPONSE_MAX_MS 20000

static const lv_image_dsc_t* _frames_idle[N_IDLE];
static const lv_image_dsc_t* _frames_listen[N_LISTEN];
static const lv_image_dsc_t* _frames_think[N_THINK];
static const lv_image_dsc_t* _frames_speak[N_SPEAK];
static const lv_image_dsc_t* _frames_response[N_RESPONSE];
static const lv_image_dsc_t* _frames_error[N_ERROR];

// Animation — timer + lv_image_set_src (objet Image standard SquareLine)
static lv_timer_t*             _anim_timer  = nullptr;
static const lv_image_dsc_t**  _play_frames = nullptr;   // ce qui défile
static uint8_t                 _play_count  = 0;
static uint8_t                 _play_idx    = 0;

// Animation de FOND, celle qu'on reprend à la fin d'un one-shot. Distincte de
// _play_frames : sans ça, le retour à l'idle après un « response » se prend
// pour un changement d'état nul et est ignoré.
static const lv_image_dsc_t**  _loop_frames = nullptr;
static uint8_t                 _loop_count  = 0;
static uint32_t                _loop_ms     = 0;

static bool     _oneshot      = false;
static uint32_t _next_resp_ms = 0;

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
    _dsc[idx].header.cf    = LV_COLOR_FORMAT_RGB565;
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
    _play_frames = nullptr;
    _loop_frames = nullptr;
    _oneshot     = false;
}

// ⚠️ Le timer est RECYCLÉ, jamais détruit ici : _play() est appelé depuis le
// callback du timer lui-même à la fin d'un one-shot.
static void _play(const lv_image_dsc_t** frames, uint8_t count, uint32_t frame_ms) {
    _play_frames = frames;
    _play_count  = count;
    _play_idx    = 0;
    lv_image_set_src(ui_ImageCompanion, frames[0]);
    if (_anim_timer) {
        lv_timer_set_period(_anim_timer, frame_ms);
        lv_timer_reset(_anim_timer);
    } else {
        _anim_timer = lv_timer_create(_anim_timer_cb, frame_ms, nullptr);
    }
}

static void _schedule_response() {
    _next_resp_ms = millis() + RESPONSE_MIN_MS +
                    (esp_random() % (RESPONSE_MAX_MS - RESPONSE_MIN_MS + 1));
}

static void _start_anim(const lv_image_dsc_t** frames, uint8_t count, uint32_t frame_ms) {
    // Un one-shot en cours doit être coupé même si l'anim de fond ne change pas
    if (_loop_frames == frames && !_oneshot) return;

    _oneshot     = false;
    _loop_frames = frames;
    _loop_count  = count;
    _loop_ms     = frame_ms;
    _play(frames, count, frame_ms);
}

// ---- SÉQUENCE D'INITIALISATION ----

static bool _load_all_frames() {
    bool all_ok = true;
    for (uint8_t i = 0; i < FRAME_COUNT; i++) {
        if (!_load_frame(i)) all_ok = false;
    }
    for (uint8_t i = 0; i < N_IDLE;     i++) _frames_idle[i]     = &_dsc[IDX_IDLE + i];
    for (uint8_t i = 0; i < N_LISTEN;   i++) _frames_listen[i]   = &_dsc[IDX_LISTEN + i];
    for (uint8_t i = 0; i < N_THINK;    i++) _frames_think[i]    = &_dsc[IDX_THINK + i];
    for (uint8_t i = 0; i < N_SPEAK;    i++) _frames_speak[i]    = &_dsc[IDX_SPEAK + i];
    for (uint8_t i = 0; i < N_RESPONSE; i++) _frames_response[i] = &_dsc[IDX_RESPONSE + i];
    for (uint8_t i = 0; i < N_ERROR;    i++) _frames_error[i]    = &_dsc[IDX_ERROR + i];
    return all_ok;
}

// ---- API LOCALES ----

static void _anim_timer_cb(lv_timer_t* t) {
    if (!_play_frames || _play_count == 0) return;

    if (++_play_idx >= _play_count) {
        _play_idx = 0;
        if (_oneshot) {                    // one-shot terminé : retour à la boucle
            _oneshot = false;
            _schedule_response();
            _play(_loop_frames, _loop_count, _loop_ms);
            return;
        }
    }
    lv_image_set_src(ui_ImageCompanion, _play_frames[_play_idx]);

    // Variation périodique, au repos seulement
    if (!_oneshot && _loop_frames == _frames_idle && millis() >= _next_resp_ms) {
        _oneshot = true;
        _play(_frames_response, N_RESPONSE, MS_RESPONSE);
    }
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
            if (_loop_frames != _frames_idle) _schedule_response();
            _start_anim(_frames_idle, N_IDLE, MS_IDLE);
            break;
        case AI_LISTENING:
            _start_anim(_frames_listen, N_LISTEN, MS_LISTEN);
            break;
        case AI_THINKING:
            _start_anim(_frames_think, N_THINK, MS_THINK);
            break;
        case AI_SPEAKING:
            _start_anim(_frames_speak, N_SPEAK, MS_SPEAK);
            break;
        case AI_ERROR:
            _start_anim(_frames_error, N_ERROR, MS_ERROR);
            break;
    }
}

void ai_companion_pause() {
    _stop_anim();   // libère le timer tant que l'écran Companion n'est pas affiché
}

void ai_companion_resume() {
    ai_companion_set_state(_last_state);
}
