// ============================================================
// AI_COMPANION.CPP — avatar 120x120 sur ui_ImageCompanion (ui_ScreenAI)
//
// Frames chargées depuis LittleFS (/companion/*.bin) en PSRAM au démarrage,
// allocation unique réutilisée pour toute la durée de vie.
//
// Format des .bin : PAS de header — les octets bruts RGB565 (120*120*2), sans
// plan alpha. Le lv_image_dsc_t est reconstruit ici.
//
// Une ANIMATION par entrée de _anims[] : préfixe, état servi, cadence. Les
// frames sont sondées sur le FS (`prefix_1`, `prefix_2`…) jusqu'au premier
// trou — ajouter une animation ne demande qu'une ligne et des fichiers.
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
// Géométrie dans ai_companion.h ; l'empreinte PSRAM, elle, se lit au runtime
// par ai_companion_psram_bytes().
#define FRAME_W        COMPANION_FRAME_W
#define FRAME_H        COMPANION_FRAME_H
#define FRAME_TOTAL_SZ (FRAME_W * FRAME_H * 2)   // plan RGB565, 28800 octets

static_assert(FRAME_TOTAL_SZ == COMPANION_FRAME_SZ,
              "COMPANION_FRAME_SZ (ai_companion.h) doit suivre le plan RGB565");

// Une animation. `base`/`count` sont DÉCOUVERTS au chargement — les écrire à la
// main, c'est rouvrir la divergence que ce tableau supprime.
// interlude : jouée en ONE-SHOT pendant que l'animation de `state` boucle,
// à intervalle tiré dans [gap_min, gap_max].
struct Anim {
    const char* prefix;
    AiState     state;
    bool        interlude;
    uint32_t    ms;
    uint32_t    gap_min, gap_max;
    uint8_t     base, count;
};

static Anim _anims[] = {
    { "idle",     AI_IDLE,      false, 130 },
    { "listen",   AI_LISTENING, false,  90 },
    { "think",    AI_THINKING,  false, 110 },
    { "speak",    AI_SPEAKING,  false,  80 },
    { "error",    AI_ERROR,     false,  90 },
    { "response", AI_IDLE,      true,  110, 8000, 20000 },
};

// En PSRAM, dimensionné au compte réel : en statique, ces descripteurs
// occupaient de la DRAM pour rien.
static lv_image_dsc_t* _dsc         = nullptr;
static uint16_t        _frame_total = 0;
static bool            _loaded      = false;

// Animation — timer + lv_image_set_src (objet Image standard SquareLine)
static lv_timer_t* _anim_timer = nullptr;
static const Anim* _playing    = nullptr;   // ce qui défile
static uint8_t     _play_idx   = 0;

// Animation de FOND, celle qu'on reprend à la fin d'un one-shot. Distincte de
// _playing : sans ça, le retour à l'idle après un intermède se prend pour un
// changement d'état nul et est ignoré.
static const Anim* _looping   = nullptr;
static const Anim* _interlude = nullptr;    // intermède de _looping, s'il en a un

static bool     _oneshot           = false;
static uint32_t _next_interlude_ms = 0;

// Dernier état demandé, pour reprendre exactement là où on était
static AiState _last_state = AI_IDLE;

// Avant-déclaration — définie en API LOCALES, appelée depuis _start_anim()
static void _anim_timer_cb(lv_timer_t* t);

// ---- HELPERS ----

// Échec silencieux + log si un fichier manque : l'avatar reste figé plutôt
// que de planter l'écran Companion.
static bool _load_frame(uint16_t idx, const char* path) {
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

// Animation servant un état, ou intermède de cet état. nullptr si le tableau
// n'en déclare pas — tous les appelants le testent.
static const Anim* _find(AiState state, bool interlude) {
    for (auto const& a : _anims)
        if (a.state == state && a.interlude == interlude && a.count > 0) return &a;
    return nullptr;
}

static void _stop_anim() {
    if (_anim_timer) {
        lv_timer_del(_anim_timer);
        _anim_timer = nullptr;
    }
    _playing   = nullptr;
    _looping   = nullptr;
    _interlude = nullptr;
    _oneshot   = false;
}

// ⚠️ Le timer est RECYCLÉ, jamais détruit ici : _play() est appelé depuis le
// callback du timer lui-même à la fin d'un one-shot.
static void _play(const Anim* a) {
    _playing  = a;
    _play_idx = 0;
    lv_image_set_src(ui_ImageCompanion, &_dsc[a->base]);
    if (_anim_timer) {
        lv_timer_set_period(_anim_timer, a->ms);
        lv_timer_reset(_anim_timer);
    } else {
        _anim_timer = lv_timer_create(_anim_timer_cb, a->ms, nullptr);
    }
}

static void _schedule_interlude() {
    if (!_interlude) return;
    _next_interlude_ms = millis() + _interlude->gap_min +
                         (esp_random() % (_interlude->gap_max - _interlude->gap_min + 1));
}

static void _start_anim(const Anim* a) {
    // Un one-shot en cours doit être coupé même si l'anim de fond ne change pas
    if (_looping == a && !_oneshot) return;

    _oneshot   = false;
    _looping   = a;
    _interlude = _find(a->state, true);
    _schedule_interlude();
    _play(a);
}

// ---- SÉQUENCE D'INITIALISATION ----

static void _frame_path(char* out, size_t n, const Anim& a, uint8_t num) {
    snprintf(out, n, "/companion/%s_%u.bin", a.prefix, (unsigned)num);
}

// Sondage : `prefix_1`, `prefix_2`… jusqu'au premier absent. La numérotation
// étant contiguë depuis 1 (le pipeline renumérote), pas besoin de lister le
// répertoire — ce qui évite au passage le tri lexical, où speak_10 précède
// speak_2.
static void _discover() {
    char path[64];
    _frame_total = 0;
    for (auto& a : _anims) {
        a.base  = (uint8_t)_frame_total;
        a.count = 0;
        while (_frame_total < COMPANION_FRAME_MAX) {
            _frame_path(path, sizeof(path), a, a.count + 1);
            if (!littlefs_exists(path)) break;
            a.count++;
            _frame_total++;
        }
    }
}

static bool _load_all_frames() {
    _discover();
    if (_frame_total == 0) {
        log_line("[Companion] Aucune frame dans /companion — 'pio run -t uploadfs' ?");
        return false;
    }
    if (_frame_total >= COMPANION_FRAME_MAX)
        log_line("[Companion] Plafond de %d frames atteint — animations tronquées",
                 COMPANION_FRAME_MAX);

    _dsc = (lv_image_dsc_t*)heap_caps_calloc(_frame_total, sizeof(lv_image_dsc_t),
                                             MALLOC_CAP_SPIRAM);
    if (!_dsc) {
        log_line("[Companion] Alloc PSRAM des descripteurs échouée");
        return false;
    }

    bool all_ok = true;
    char path[64];
    for (auto const& a : _anims) {
        // ⚠️ Une animation déclarée mais sans fichier ferait indexer un tableau
        // vide dans _play(). Elle est signalée et tout le chargement échoue.
        if (a.count == 0) {
            log_line("[Companion] Animation '%s' sans aucune frame", a.prefix);
            all_ok = false;
            continue;
        }
        for (uint8_t i = 0; i < a.count; i++) {
            _frame_path(path, sizeof(path), a, i + 1);
            if (!_load_frame(a.base + i, path)) all_ok = false;
        }
    }
    return all_ok;
}

// ---- API LOCALES ----

static void _anim_timer_cb(lv_timer_t* t) {
    if (!_playing || _playing->count == 0) return;

    if (++_play_idx >= _playing->count) {
        _play_idx = 0;
        if (_oneshot) {                    // one-shot terminé : retour à la boucle
            _oneshot = false;
            _schedule_interlude();
            _play(_looping);
            return;
        }
    }
    lv_image_set_src(ui_ImageCompanion, &_dsc[_playing->base + _play_idx]);

    // Variation périodique, sur les états qui déclarent un intermède
    if (!_oneshot && _interlude && millis() >= _next_interlude_ms) {
        _oneshot = true;
        _play(_interlude);
    }
}

// ---- API PUBLIQUES ----

void ai_companion_init() {
    _loaded = _load_all_frames();
    if (!_loaded) {
        log_line("[Companion] Chargement incomplet — vérifie 'pio run -t uploadfs'");
        return;   // évite de pointer lv_image_set_src sur un buffer nullptr
    }
    lv_image_set_src(ui_ImageCompanion, &_dsc[0]);   // avatar figé, pas vide

    // Une seule ligne : le journal circulaire ne fait que 40 lignes.
    // ⚠️ snprintf rend la longueur VOULUE : sans la borne, assez d'animations
    // feraient passer le reste à négatif et déborderaient `detail`.
    char   detail[LOG_LINE_LEN];
    size_t n = 0;
    for (auto const& a : _anims) {
        if (n + 1 >= sizeof(detail)) break;
        int w = snprintf(detail + n, sizeof(detail) - n, "%s:%u ", a.prefix, (unsigned)a.count);
        if (w < 0) break;
        n = min(n + (size_t)w, sizeof(detail) - 1);
    }
    // Résumé D'ABORD : avec assez d'animations, c'est le détail qui se fait
    // tronquer par LOG_LINE_LEN, pas le total.
    log_line("[Companion] %u frames, %u Ko — %s", (unsigned)_frame_total,
             (unsigned)(ai_companion_psram_bytes() / 1024), detail);
}

void ai_companion_set_state(AiState state) {
    if (!_loaded) return;   // pas de frames en mémoire, rien à afficher
    _last_state = state;
    const Anim* a = _find(state, false);
    if (a) _start_anim(a);
}

uint32_t ai_companion_psram_bytes() {
    return (uint32_t)_frame_total * COMPANION_FRAME_SZ;
}

void ai_companion_pause() {
    _stop_anim();   // libère le timer tant que l'écran Companion n'est pas affiché
}

void ai_companion_resume() {
    ai_companion_set_state(_last_state);
}
