// ============================================================
// AUDIO_MANAGER.CPP — Codec ES8311 (I2C 0x18) + I2S full-duplex 16 kHz
//   TX → ampli FM8002E → HP     RX ← ADC ES8311 ← micro
// Tout le rendu (bloquant) tourne sur _audio_task, cœur 1.
//
// ⚠️ La flash n'est dans AUCUN chemin audio : une écriture suspend le cache
// d'instructions et fige LVGL (~3 s pour 190 Ko). TTS joué depuis la PSRAM,
// captures envoyées au NAS. Ne pas réintroduire d'écriture ici.
// ============================================================

// ---- RESSOURCES BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>   // API I2S Arduino 3.x — partagée avec ESP_SR
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <math.h>
#include <esp_heap_caps.h>

// ---- RESSOURCES LOCALES ----
#include "audio_manager.h"
#include "config.h"
#include "ai_manager.h"         // ai_upload_pcm
#include "log_manager.h"
#include "wakeword_manager.h"

// ---- OBJETS GLOBAUX ----
#define ES8311_ADDR        0x18
#define SAMPLE_RATE        AUDIO_SAMPLE_RATE   // référence unique dans config.h
// La capacité d'enregistrement (durée max + 1 s de marge) vit dans
// audio_manager.h : la page MEMOIRE de SysInfo la lit aussi.
#define LOOPBACK_RECORD_MS 3000

// Purge du HP avant d'enregistrer (cf. _drain_playback)
#define PLAYBACK_DRAIN_SILENCE_MS  120
#define PLAYBACK_DRAIN_WAIT_MS      60

#define SPEECH_MIN_MS        250   // parole cumulée requise pour armer la coupure au silence
#define NO_SPEECH_TIMEOUT_MS 3000  // rien entendu au bout de ça → abandon

bool audio_is_recording = false;
bool audio_is_playing   = false;

static I2SClass _i2s;
static uint8_t  _volume = 215;   // registre 0x32 : 0,5 dB/pas, 215 ≈ -17 dB

// Tampons de travail en RAM INTERNE (.bss) — hors des fonctions, pour rester
// visibles à l'inventaire de l'interne.
static int16_t _audio_buf[1024 * 2];   // conversion mono -> stéréo avant write (4 Ko)
static int16_t _stereo_buf[256 * 2];   // trame brute lue depuis l'ADC (1 Ko)

static volatile bool _cancel_requested = false;
static volatile bool _stop_requested   = false;
static volatile bool _playback_stop_requested = false;

static int16_t* _record_psram_buf = nullptr;   // alloué une fois, cf. _ensure_record_buffer

// Flux TTS — tampon FreeRTOS à stockage PSRAM (le contrôle reste en interne).
// Producteur ai_task, consommateur audio_task : un seul de chaque, ce que le
// stream buffer exige.
static StreamBufferHandle_t _stream_buf   = nullptr;
static StaticStreamBuffer_t _stream_ctrl;
static uint8_t*             _stream_store = nullptr;
static volatile bool        _stream_eof   = false;

#define STREAM_PREBUFFER_TIMEOUT_MS 4000   // départ forcé si le débit ne suit pas
#define STREAM_PUSH_TIMEOUT_MS      5000   // au-delà, la lecture est morte

enum AudioCmd {
    AUDIO_CMD_CLICK,
    AUDIO_CMD_FF6,
    AUDIO_CMD_TEST_LOOPBACK,
    AUDIO_CMD_RECORD_FILE,
    AUDIO_CMD_PLAY_PSRAM_STREAM,
    AUDIO_CMD_PLAY_STREAM,
    AUDIO_CMD_WAKEWORD_ACK,
};

// ⚠️ Chaque message porte SON payload : avec des statics partagés, deux STREAM
// empilés reliraient le MÊME buffer -> double-free -> panic.
struct AudioMsg {
    AudioCmd             cmd;
    const int16_t*       buf;         // PLAY_PSRAM_STREAM
    size_t               samples;     // PLAY_PSRAM_STREAM
    bool                 free_after;  // PLAY_PSRAM_STREAM
    int                  max_ms;      // RECORD_FILE
    audio_record_done_cb done_cb;     // RECORD_FILE
};

static QueueHandle_t _audio_queue = nullptr;

// ---- HELPERS ----

static void _es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t _es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    unsigned long t = millis();
    while (!Wire.available()) {
        if (millis() - t > 5) return 0;
    }
    return Wire.read();
}

static void _play_tone(float freq, int ms, float amp = 0.4f) {
    int total_samples = (SAMPLE_RATE * ms) / 1000;
    for (int pos = 0; pos < total_samples; ) {
        int chunk = min(256, total_samples - pos);
        for (int i = 0; i < chunk; i++) {
            float t   = (float)(pos + i) / SAMPLE_RATE;
            float env = sinf(((float)(pos + i) / total_samples) * M_PI);
            int16_t v = (int16_t)(sinf(2.0f * M_PI * freq * t) * 30000 * amp * env);
            _audio_buf[i * 2]     = v;
            _audio_buf[i * 2 + 1] = v;
        }
        _i2s.write((uint8_t*)_audio_buf, chunk * 2 * sizeof(int16_t));
        pos += chunk;
    }
}

static void _play_silence(int ms) {
    memset(_audio_buf, 0, sizeof(_audio_buf));
    int samples = (SAMPLE_RATE * ms) / 1000;
    while (samples > 0) {
        int chunk = min(256, samples);
        _i2s.write((uint8_t*)_audio_buf, chunk * 2 * sizeof(int16_t));
        samples -= chunk;
    }
}

// _i2s.write() rend la main avant que le HP ait fini d'émettre : sans cette
// purge, le micro capte la fin du jingle et Whisper hallucine dessus.
static void _drain_playback() {
    _play_silence(PLAYBACK_DRAIN_SILENCE_MS);
    delay(PLAYBACK_DRAIN_WAIT_MS);
}

static bool _ensure_record_buffer() {
    if (_record_psram_buf) return true;
    _record_psram_buf = (int16_t*)heap_caps_malloc(
        AUDIO_RECORD_CAPACITY_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!_record_psram_buf) {
        log_line("[Audio] Impossible d'allouer le buffer PSRAM d'enregistrement");
        return false;
    }
    return true;
}

// ---- SÉQUENCE D'INITIALISATION ----

// Ordre imposé par la datasheet §9.1 : reset -> horloges -> format -> power-up
// -> ADC -> DAC.
static void _es8311_init() {
    _es_write(0x00, 0x1F);      // reset, CSM_ON=0
    delay(20);
    _es_write(0x00, 0x00);      // relâche les resets
    _es_write(0x00, 0x80);      // CSM_ON=1, mode normal
    _es_write(0x01, 0x3F);      // MCLK/BCLK/ADC/DAC clocks ON
    _es_write(0x02, 0x00);      // ÷1 ×1 — MCLK 4,096 MHz (256×Fs) fourni par ESP_I2S.
                                // Seul registre à changer vs l'ancien 768× (0x03..0x08 identiques).
    _es_write(0x03, 0x10);      // ADC single speed, OSR=16
    _es_write(0x04, 0x10);      // DAC single speed, OSR=16
    _es_write(0x05, 0x00);      // horloges ADC/DAC = horloge interne
    _es_write(0x06, 0x03);      // BCLK continu (DIV ignoré en slave)
    _es_write(0x07, 0x00);      // mode normal, DIV_LRCK hi (ignoré en slave)
    _es_write(0x08, 0xFF);      // DIV_LRCK lo (ignoré en slave)
    uint8_t reg00 = _es_read(0x00) & 0xBF;
    _es_write(0x00, reg00);     // mode SLAVE I2S, garde CSM_ON=1

    _es_write(0x09, 0x0C);  // I2S 16 bits, entrée DAC canal gauche
    _es_write(0x0A, 0x0C);  // I2S 16 bits, sortie ADC
    _es_write(0x0D, 0x01);  // power-up des blocs analogiques
    _es_write(0x0E, 0x02);  // PGA + modulateur ADC ON
    _es_write(0x12, 0x00);  // DAC ON
    _es_write(0x13, 0x10);  // ampli casque OFF (on utilise OUTP/OUTN + FM8002E)
    _es_write(0x1C, 0x6A);  // passe-haut ADC (supprime l'offset DC)
    _es_write(0x14, 0x1A);  // Mic1P–Mic1N, PGA analogique +30 dB (max matériel)
    // ⚠️ Micro volontairement LINÉAIRE — NE PAS RÉACTIVER L'ALC ni l'automute.
    // ALC coupée, 0x17 n'est plus un plafond mais le gain numérique FIXE :
    // 0xFF (+32 dB) obligatoire, à 0 dB la parole tombe à -52 dBFS.
    _es_write(0x17, 0xFF);
    _es_write(0x18, 0x00);  // ALC_EN=0, ADC_AUTOMUTE_EN=0
    _es_write(0x19, 0xF0);  // cibles ALC — sans effet
    _es_write(0x1A, 0x37);  // fenêtre automute — sans effet
    _es_write(0x37, 0x08);  // DAC : softramp off, EQ bypassé
    _es_write(0x32, _volume);
}

// Full-duplex : setPins() donne dout ET din, begin() alloue les deux canaux sur
// le même port. Stéréo 16 bits, micro sur le canal gauche. ES8311 en SLAVE.
static void _i2s_init() {
    _i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_DIN, I2S_MCLK);
    if (!_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        log_line("[Audio] Echec _i2s.begin()");
    }
}

// ---- API LOCALES ----

// Capture vers la PSRAM. silence_cutoff : coupe après ~800 ms sous le seuil,
// mais SEULEMENT après SPEECH_MIN_MS de vraie parole.
// *out_cancelled distingue l'annulation (on jette) de l'arrêt manuel (on garde).
static bool _audio_capture_to_psram(int max_ms, bool silence_cutoff, size_t* out_samples,
                                     bool* out_cancelled, uint16_t* out_speech_ms = nullptr) {
    *out_samples   = 0;
    *out_cancelled = false;
    if (out_speech_ms) *out_speech_ms = 0;

    if (!_ensure_record_buffer()) return false;

    int total_frames = (SAMPLE_RATE * max_ms) / 1000;
    if (total_frames > AUDIO_RECORD_CAPACITY_SAMPLES) {
        total_frames = AUDIO_RECORD_CAPACITY_SAMPLES;
        log_line("[Audio] max_ms tronqué à la capacité du buffer PSRAM");
    }

    const int16_t SILENCE_THRESHOLD  = 400;
    const int     SILENCE_MS_TO_STOP = 800;
    int silence_frames = 0;
    int silence_limit  = (SAMPLE_RATE * SILENCE_MS_TO_STOP) / 1000;

    // Cumul (et non drapeau) : un bruit bref ne doit pas passer pour de la parole.
    int speech_frames   = 0;
    int speech_min      = (SAMPLE_RATE * SPEECH_MIN_MS) / 1000;
    int no_speech_limit = (SAMPLE_RATE * NO_SPEECH_TIMEOUT_MS) / 1000;

    size_t  recorded = 0;
    int16_t min_l = 32767, max_l = -32768;

    _cancel_requested = false;
    uint32_t t_loop_start = millis();

    while (recorded < (size_t)total_frames) {
        size_t want = min((size_t)256, (size_t)total_frames - recorded);

        size_t bytes_read  = _i2s.readBytes((char*)_stereo_buf, want * 2 * sizeof(int16_t));
        size_t frames_read = bytes_read / (2 * sizeof(int16_t));
        if (frames_read == 0) continue;

        int32_t sum_abs = 0;
        for (size_t i = 0; i < frames_read; i++) {
            int16_t l = _stereo_buf[i * 2];
            _record_psram_buf[recorded + i] = l;
            if (l < min_l) min_l = l;
            if (l > max_l) max_l = l;
            sum_abs += abs((int)l);
        }
        recorded += frames_read;

        if (_cancel_requested || _stop_requested) break;

        if (silence_cutoff) {
            int16_t avg = (int16_t)(sum_abs / frames_read);
            if (avg < SILENCE_THRESHOLD) {
                silence_frames += frames_read;
                if (speech_frames >= speech_min && silence_frames > silence_limit) break;
                if (speech_frames < speech_min && (int)recorded > no_speech_limit) {
                    log_line("[Audio] Aucune parole détectée en %d ms — abandon", NO_SPEECH_TIMEOUT_MS);
                    break;
                }
            } else {
                speech_frames += frames_read;
                silence_frames = max(0, silence_frames - (int)frames_read * 3);
            }
        }
    }

    uint32_t wall_ms  = millis() - t_loop_start;
    uint32_t audio_ms = (uint32_t)((uint64_t)recorded * 1000 / SAMPLE_RATE);

    log_line("[Audio] Capture I2S RX : %lu ms (plafond %d ms) — %u ech., parole %lu ms",
             (unsigned long)wall_ms, max_ms, (unsigned)recorded,
             (unsigned long)((uint64_t)speech_frames * 1000 / SAMPLE_RATE));

    // La capture est TEMPS RÉEL : temps écoulé >> durée audio = tampon DMA
    // débordé. Rien d'autre ne le signale, l'amplitude reste normale.
    if (audio_ms > 0 && wall_ms > audio_ms + audio_ms / 4) {
        log_line("[Audio] ALERTE : %lu ms réel / %lu ms audio (x%lu.%02lu) — échantillons PERDUS",
                 (unsigned long)wall_ms, (unsigned long)audio_ms,
                 (unsigned long)(wall_ms / audio_ms),
                 (unsigned long)((wall_ms * 100 / audio_ms) % 100));
    }
    log_line("[Audio] Amplitude capturée : min=%d max=%d", min_l, max_l);

    if (out_speech_ms) {
        uint32_t ms = (uint32_t)((uint64_t)speech_frames * 1000 / SAMPLE_RATE);
        *out_speech_ms = (uint16_t)min(ms, (uint32_t)UINT16_MAX);
    }

    *out_cancelled = _cancel_requested;
    *out_samples   = recorded;
    return true;
}

static void _audio_play_psram_stream(const int16_t* pcm, size_t samples, bool free_after) {
    audio_is_playing = true;
    _playback_stop_requested = false;

    size_t remaining = samples;
    const int16_t* p = pcm;
    while (remaining > 0) {
        if (_playback_stop_requested) break;
        size_t n = min((size_t)1024, remaining);
        for (size_t i = 0; i < n; i++) {
            _audio_buf[i * 2]     = p[i];
            _audio_buf[i * 2 + 1] = p[i];
        }
        _i2s.write((uint8_t*)_audio_buf, n * 2 * sizeof(int16_t));
        p         += n;
        remaining -= n;
    }
    audio_is_playing = false;

    if (free_after) free((void*)pcm);
}

// Lecture EN FLUX. Une seule session, donc un seul wakeword_pause/resume et un
// seul front descendant de audio_is_playing — c'est toute la raison d'être de
// ce chemin séparé plutôt que d'empiler des PLAY_PSRAM_STREAM.
static void _audio_play_stream() {
    audio_is_playing = true;
    _playback_stop_requested = false;

    // Pré-remplissage : sans lui le DMA se vide au premier à-coup réseau.
    uint32_t t0 = millis();
    while (!_stream_eof && !_playback_stop_requested &&
           xStreamBufferBytesAvailable(_stream_buf) < AUDIO_STREAM_PREBUFFER &&
           millis() - t0 < STREAM_PREBUFFER_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Transit dans la MOITIÉ HAUTE de _audio_buf, l'expansion stéréo remplit la
    // basse. Sûr en montant : on n'écrit par-dessus l'octet source qu'au tout
    // dernier échantillon, déjà recopié dans `v`. Évite un statique dédié, donc
    // 2 Ko d'interne prélevés sur le plancher.
    const size_t   cap  = 1024;                       // échantillons par bloc
    int16_t* const mono = _audio_buf + cap;
    size_t played = 0, underruns = 0;
    uint8_t carry = 0;
    bool    has_carry = false;

    for (;;) {
        if (_playback_stop_requested) break;

        // ⚠️ Le flux n'est pas aligné sur 2 octets : ce qui arrive dépend du
        // découpage TCP. L'octet orphelin est reporté au bloc suivant, sinon
        // tout le reste du flux est décalé d'un octet.
        // ⚠️ has_carry n'est levé qu'APRÈS réception : sinon un octet orphelin
        // en fin de flux empêche la boucle de sortir (bytes impair -> n=0 ->
        // on repart), et audio_task tourne sans fin, wakeword jamais relancé.
        uint8_t* dst = (uint8_t*)mono;
        size_t   off = has_carry ? 1 : 0;
        if (off) dst[0] = carry;

        size_t got = xStreamBufferReceive(_stream_buf, dst + off,
                                          cap * sizeof(int16_t) - off,
                                          pdMS_TO_TICKS(50));
        if (got == 0) {
            if (_stream_eof) break;   // un demi-échantillon en attente est perdu, sans effet
            if (!off) underruns++;
            continue;
        }
        has_carry = false;
        size_t bytes = got + off;
        if (bytes & 1) {              // demi-échantillon en fin de bloc
            carry = dst[bytes - 1];
            has_carry = true;
            bytes--;
        }

        size_t n = bytes / sizeof(int16_t);
        for (size_t i = 0; i < n; i++) {
            int16_t v = mono[i];
            _audio_buf[i * 2]     = v;
            _audio_buf[i * 2 + 1] = v;
        }
        if (n) _i2s.write((uint8_t*)_audio_buf, n * 2 * sizeof(int16_t));
        played += n;
    }

    audio_is_playing = false;
    // Pas de %f : newlib nano ne le gère pas.
    unsigned ms = (unsigned)((uint64_t)played * 1000 / AUDIO_SAMPLE_RATE);
    // underruns > 0 = le tampon s'est vidé : c'est ce qui s'entend en clics.
    log_line("[Audio] Flux joué : %u,%u s (%u ech.), %u creux",
             ms / 1000, (ms % 1000) / 100, (unsigned)played, (unsigned)underruns);
}

static void _sound_click() {
    _play_tone(1200.0f, 30, 0.5f);
}

// Accusé de réception avant l'écoute. Drain indispensable : l'enregistrement
// est empilé juste derrière dans la file.
static void _sound_wakeword_ack() {
    _play_tone(660.0f, 90, 0.45f);
    _play_silence(20);
    _play_tone(990.0f, 130, 0.45f);
    _drain_playback();
}

// Fanfare victoire Final Fantasy 6
static void _sound_ff6() {
    struct Note { float freq; int dur; };
    const Note melody[] = {
        {2349, 53}, {0,   53},
        {2349, 53}, {0,   53},
        {2349, 53}, {0,   53},
        {2349,428},
        { 932,428},
        {2093,428},
        {2349,107}, {0,  214},
        {2093,107},
        {2349,857}
    };
    for (auto const &n : melody) {
        if (n.freq == 0) _play_silence(n.dur);
        else             _play_tone(n.freq, n.dur);
    }
}

// Test hardware micro + HP : enregistre, rejoue, archive sur le NAS.
// Envoi asynchrone (file de _ai_task), il ne retarde pas le bus I2S.
static void _sound_test_loopback() {
    log_line("[Audio] Test loopback — enregistrement %d ms...", LOOPBACK_RECORD_MS);

    size_t samples   = 0;
    bool   cancelled = false;

    audio_is_recording = true;
    _audio_capture_to_psram(LOOPBACK_RECORD_MS, false, &samples, &cancelled);
    audio_is_recording = false;

    if (samples == 0) {
        log_line("[Audio] Test loopback : aucune donnée capturée");
        return;
    }

    uint32_t t_play = millis();
    _audio_play_psram_stream(_record_psram_buf, samples, false);   // false : buffer singleton
    log_line("[Audio] Lecture : %lu ms (%u échantillons)",
             (unsigned long)(millis() - t_play), (unsigned)samples);

    ai_upload_pcm(_record_psram_buf, samples);
    log_line("[Audio] Test loopback terminé — capture envoyée au NAS");
}

// Enregistrement IA — le buffer PSRAM part directement chez ai_manager.
static void _audio_record(int max_ms, audio_record_done_cb done_cb) {
    size_t   samples   = 0;
    bool     cancelled = false;
    uint16_t speech_ms = 0;

    audio_is_recording = true;
    log_line("[Audio] Enregistrement IA démarré (max %d ms)", max_ms);
    bool ok = _audio_capture_to_psram(max_ms, true, &samples, &cancelled, &speech_ms);
    audio_is_recording = false;

    log_line("[Audio] Enregistrement IA terminé : %u échantillons%s",
             (unsigned)samples, cancelled ? " (annulé)" : "");

    _cancel_requested = false;
    _stop_requested   = false;

    if (done_cb) {
        done_cb(_record_psram_buf, samples, cancelled, ok && samples > 0 && !cancelled, speech_ms);
    }
}

// ESP_SR écoute le micro au repos : pause le temps de la commande, pour
// garantir un seul lecteur I2S à la fois.
static void _audio_task(void* pvParameters) {
    AudioMsg msg;
    for (;;) {
        if (xQueueReceive(_audio_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        wakeword_pause();
        switch (msg.cmd) {
            case AUDIO_CMD_CLICK:          _sound_click();         break;
            case AUDIO_CMD_FF6:            _sound_ff6();           break;
            case AUDIO_CMD_TEST_LOOPBACK:  _sound_test_loopback(); break;
            case AUDIO_CMD_RECORD_FILE:    _audio_record(msg.max_ms, msg.done_cb); break;
            case AUDIO_CMD_WAKEWORD_ACK:   _sound_wakeword_ack();  break;
            case AUDIO_CMD_PLAY_PSRAM_STREAM:
                _audio_play_psram_stream(msg.buf, msg.samples, msg.free_after);
                break;
            case AUDIO_CMD_PLAY_STREAM:    _audio_play_stream();   break;
        }
        wakeword_resume();

        log_line("[Audio] Pile restante (audio_task) : %u octets",
                 (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
}

// ---- API PUBLIQUES ----

void audio_init() {
    pinMode(AMP_ENABLE, OUTPUT);
    digitalWrite(AMP_ENABLE, LOW);

    _i2s_init();
    delay(10);
    _es8311_init();

    _ensure_record_buffer();   // évite un malloc PSRAM au premier enregistrement

    _audio_queue = xQueueCreate(4, sizeof(AudioMsg));
    // Pile : STACK_BYTES_AUDIO_TASK.
    xTaskCreatePinnedToCore(_audio_task, "audio_task", STACK_BYTES_AUDIO_TASK, nullptr, 3, nullptr, 1);

    log_line("[Audio] Init OK");
}

void audio_loop() {}

void audio_set_volume(int percent) {
    percent = constrain(percent, 0, 100);
    const float DB_MIN = -40.0f;
    const float DB_MAX =  19.0f;
    if (percent == 0) {
        _volume = 0;
    } else {
        float db = DB_MIN + (percent / 100.0f) * (DB_MAX - DB_MIN);
        _volume = (uint8_t)((db + 95.5f) / 0.5f);   // dB -> registre
    }
    _es_write(0x32, _volume);
}

I2SClass& audio_get_i2s() {
    return _i2s;
}

void audio_click() {
    if (!_audio_queue) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_CLICK;
    xQueueSend(_audio_queue, &msg, 0);
}

void audio_wakeword_ack() {
    if (!_audio_queue) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_WAKEWORD_ACK;
    xQueueSend(_audio_queue, &msg, 0);
}

void audio_ff6() {
    if (!_audio_queue) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_FF6;
    xQueueSend(_audio_queue, &msg, 0);
}

void audio_test_loopback() {
    if (!_audio_queue) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_TEST_LOOPBACK;
    xQueueSend(_audio_queue, &msg, 0);
}

void audio_play_psram_stream_queue(const int16_t* psram_buf, size_t samples, bool free_after) {
    if (!_audio_queue || !psram_buf) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_PLAY_PSRAM_STREAM;
    msg.buf = psram_buf; msg.samples = samples; msg.free_after = free_after;
    xQueueSend(_audio_queue, &msg, portMAX_DELAY);
}

// ⚠️ Le stockage doit faire xBufferSizeBytes + 1 : le stream buffer FreeRTOS
// garde une case morte pour distinguer plein de vide.
static bool _ensure_stream_buffer() {
    if (_stream_buf) return true;
    _stream_store = (uint8_t*)heap_caps_malloc(AUDIO_STREAM_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!_stream_store) {
        log_line("[Audio] PSRAM insuffisante pour le flux TTS (%u o)",
                 (unsigned)AUDIO_STREAM_BYTES);
        return false;
    }
    // ⚠️ Seuil de déclenchement = un bloc entier. À 1, chaque lecture rendait
    // quelques octets et l'I2S recevait des centaines d'écritures minuscules
    // par seconde : le DMA se vidait entre deux, ce qui s'entend en tics. Le
    // délai d'attente de la réception laisse quand même passer le reliquat
    // final, plus court qu'un bloc.
    _stream_buf = xStreamBufferCreateStatic(AUDIO_STREAM_BYTES, 1024 * sizeof(int16_t),
                                            _stream_store, &_stream_ctrl);
    return _stream_buf != nullptr;
}

bool audio_stream_begin() {
    if (!_audio_queue || !_ensure_stream_buffer()) return false;
    xStreamBufferReset(_stream_buf);
    _stream_eof = false;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_PLAY_STREAM;
    xQueueSend(_audio_queue, &msg, portMAX_DELAY);
    return true;
}

size_t audio_stream_push(const uint8_t* data, size_t len) {
    if (!_stream_buf) return 0;
    return xStreamBufferSend(_stream_buf, data, len, pdMS_TO_TICKS(STREAM_PUSH_TIMEOUT_MS));
}

void audio_stream_end() {
    _stream_eof = true;
}

bool audio_stream_allocated() {
    return _stream_store != nullptr;
}

void audio_record_file(int max_ms, audio_record_done_cb on_done) {
    if (!_audio_queue) return;
    AudioMsg msg = {};
    msg.cmd = AUDIO_CMD_RECORD_FILE;
    msg.max_ms = max_ms; msg.done_cb = on_done;
    xQueueSend(_audio_queue, &msg, 0);
}

void audio_cancel_recording() {
    _cancel_requested = true;
}

void audio_stop_recording() {
    _stop_requested = true;
}

void audio_stop_playback() {
    _playback_stop_requested = true;
}
