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

// Tampon du flux TTS, en PSRAM : la lecture démarre pendant la réception au
// lieu d'attendre la réponse entière. Second consommateur : _si_alloc.
//
// ⚠️ SA TAILLE EST UN ENJEU DE RAM INTERNE, pas de confort. Tampon plein,
// audio_stream_push() bloque et l'ESP32 cesse de lire le socket : lwIP retient
// alors ses pbufs — alloués en INTERNE — pendant tout le calage. Mesuré le
// 2026-08-04 avec 128 Ko : une réponse de 25,6 s cale la réception 21,7 s et
// fait tomber le plancher de 12 520 à 2 552 o d'interne.
// Le bridge synthétise à ~3,5x le temps réel, donc le retard culmine à
// 0,71 x la durée de la réponse. 32 s de capacité couvrent ~45 s de parole ;
// au-delà le contrôle de flux reprend son rôle de filet.
#define AUDIO_STREAM_BYTES     (AUDIO_SAMPLE_RATE * 2 * 32)  // 32 s d'audio, 1 Mo
// ⚠️ Le pré-remplissage absorbe la gigue réseau — le réduire expose à un DMA
// à sec, donc à des blancs au milieu de la phrase.
#define AUDIO_STREAM_PREBUFFER (AUDIO_SAMPLE_RATE * 2 * 1)   // 1 s avant de lancer

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

// Lecture EN FLUX — l'audio est joué au fil de son arrivée.
// begin() poste UN seul message sur la file audio : c'est ce qui garantit un
// unique wakeword_pause/resume et un unique front descendant de
// audio_is_playing, dont display_loop() se sert pour revenir à AI_IDLE.
// push() bloque quand le tampon est plein — c'est le contrôle de flux, il cale
// la réception HTTP sur le rythme de la lecture ; il rend moins que `len` si la
// lecture s'est arrêtée, l'appelant doit alors abandonner.
// ⚠️ Une seule session à la fois (appelant unique : ai_manager, sur ai_task).
bool   audio_stream_begin();
size_t audio_stream_push(const uint8_t* data, size_t len);
void   audio_stream_end();

// Le tampon n'est alloué qu'au premier flux : _si_alloc doit le savoir, sinon
// il le compte comme utilisé et le « non tracé » PSRAM part en négatif.
bool   audio_stream_allocated();

void audio_record_file(int max_ms, audio_record_done_cb on_done);
void audio_cancel_recording();
void audio_stop_recording();
void audio_stop_playback();
