#pragma once

// ============================================================
// AI_MANAGER.H — passerelle vers le bridge NAS.
// Envoie une requête (audio -> STT, ou texte via ai/ask), récupère
// transcript + answer + TTS, expose l'état résultant. Ne dessine rien :
// le rendu de ui_ScreenAI est dans display_manager.cpp.
//
// Tâche dédiée + file de commandes. Tout ce qui vient de _ai_task ou
// _audio_task repasse par une file drainée dans ai_loop() — jamais
// lv_async_call(), qui n'est pas thread-safe.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <stddef.h>

// ---- OBJETS GLOBAUX ----
enum AiState {
    AI_IDLE, AI_LISTENING, AI_THINKING, AI_SPEAKING, AI_ERROR
};

typedef void (*ai_state_cb)(AiState state);

// ---- API PUBLIQUES ----

void ai_init();   // après audio_init() + mqtt_init()
void ai_loop();   // depuis loop() — thread principal

void ai_start_listening();
void ai_stop_listening();         // arrête et envoie ce qui a été capturé
void ai_cancel_listening();       // jette la capture en cours
void ai_notify_speaking_done();   // quand audio_is_playing retombe à false
void ai_replay_answer();          // refait synthétiser la dernière réponse

// TTS seul : ni STT, ni LLM, ni mémorisation dans l'historique. Pour les
// invites système. Non bloquant, appelable depuis n'importe quelle tâche.
void ai_say(const char* text);

// Diagnostic — archive un PCM en WAV horodaté sur le NAS (POST /record).
// Non bloquant, ne touche pas à l'état IA.
void ai_upload_pcm(const int16_t* pcm, size_t samples);
void ai_upload_last_record();   // idem, sur la dernière capture vocale

AiState     ai_get_state();
const char* ai_get_transcript();
const char* ai_get_answer();

void ai_set_state_callback(ai_state_cb cb);

// Appelés depuis mqtt_manager.cpp, déjà sur le thread principal.
void ai_on_transcript(const char* text);
void ai_on_answer(const char* text);
void ai_on_ask_request(const char* question);
