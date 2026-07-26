#pragma once

// ============================================================
// AUDIO_MANAGER.H — ES8311 + I2S, tâche dédiée + file de commandes.
// Tout appel rend la main immédiatement, rien ne bloque loop().
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"

class I2SClass;   // fwd — évite d'inclure ESP_I2S.h partout

// ---- OBJETS GLOBAUX ----
extern bool audio_is_playing;
extern bool audio_is_recording;

// Capacité du buffer d'enregistrement PSRAM : durée max + 1 s de marge.
// Second consommateur : la page MEMOIRE de SysInfo (_si_alloc).
#define AUDIO_RECORD_CAPACITY_SAMPLES (AUDIO_SAMPLE_RATE * (AUDIO_RECORD_MAX_SECONDS + 1))

// speech_ms : durée cumulée au-dessus du seuil d'énergie. Seul critère fiable
// de "il y a eu de la parole" — le pic d'amplitude ne distingue pas un bip
// d'une phrase (cf. AUDIO_MIN_SPEECH_MS dans config.h).
typedef void (*audio_record_done_cb)(const int16_t* pcm, size_t samples, bool cancelled, bool ok,
                                      uint16_t speech_ms);

// Bus I2S partagé avec ESP_SR. Valable seulement après audio_init().
I2SClass& audio_get_i2s();

// ---- API PUBLIQUES ----

void audio_init();
void audio_loop();   // vide, tout tourne sur _audio_task
void audio_set_volume(int percent);
void audio_click();
void audio_ff6();
void audio_wakeword_ack();   // jingle avant l'écoute
void audio_test_loopback();  // test hardware micro+HP, capture archivée sur le NAS

// Lecture depuis la PSRAM. free_after=true : buffer libéré après lecture.
void audio_play_psram_stream_queue(const int16_t* psram_buf, size_t samples, bool free_after);

void audio_record_file(int max_ms, audio_record_done_cb on_done);
void audio_cancel_recording();
void audio_stop_recording();
void audio_stop_playback();
