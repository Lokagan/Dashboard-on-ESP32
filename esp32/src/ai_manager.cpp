// ============================================================
// AI_MANAGER.CPP — états de l'IA + requêtes HTTP vers le bridge NAS.
//
// L'état est partagé entre _audio_task, _ai_task et loopTask : seul
// loopTask l'applique (_apply), les autres passent par _post_state_async.
//
// L'audio uploadé reste en PSRAM (buffer singleton d'audio_manager) et
// part directement en HTTP, sans jamais toucher la flash. Il reste valable
// tant qu'un nouvel enregistrement n'est pas lancé — ce que la machine à
// états interdit pendant THINKING/SPEAKING.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>
#include <HTTPClient.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

// ---- RESSOURCES LOCALES ----
#include "ai_manager.h"
#include "audio_manager.h"
#include "mqtt_manager.h"
#include "wifi_manager.h"
#include "log_manager.h"
#include "config.h"

// ---- OBJETS GLOBAUX ----

// Filet si le bridge ne renvoie pas de Content-Length (non attendu) — 20 s.
#define PSRAM_STREAM_FALLBACK_BYTES (AUDIO_SAMPLE_RATE * 20 * sizeof(int16_t))

static AiState     _state           = AI_IDLE;
static char        _transcript[200] = "";
static char        _answer[200]     = "";
static ai_state_cb _cb              = nullptr;

struct _PostMsg {
    AiState state;
    char    transcript[200];
    char    answer[200];
    bool    has_transcript;
    bool    has_answer;
};

struct AiCmdMsg {
    enum Cmd { CMD_ASK_AUDIO, CMD_ASK_TEXT, CMD_SAY, CMD_UPLOAD_RECORD } cmd;
    const int16_t* pcm_buf;       // CMD_ASK_AUDIO / CMD_UPLOAD_RECORD
    size_t         pcm_samples;
    char           text[200];     // CMD_ASK_TEXT / CMD_SAY
};

static QueueHandle_t _ai_queue   = nullptr;
static QueueHandle_t _post_queue = nullptr;   // états à appliquer sur loopTask

static uint32_t _last_ask_request_ms = 0;
static const uint32_t ASK_REQUEST_COOLDOWN_MS = 2000;

// Dernière capture reçue, y compris celles écartées faute de parole : ce sont
// justement celles qu'on veut pouvoir inspecter.
static const int16_t* _last_pcm     = nullptr;
static size_t         _last_samples = 0;

// ---- HELPERS ----

static const char* _state_name(AiState s) {
    switch (s) {
        case AI_IDLE:      return "idle";
        case AI_LISTENING: return "listening";
        case AI_THINKING:  return "thinking";
        case AI_SPEAKING:  return "speaking";
        case AI_ERROR:     return "error";
    }
    return "idle";
}

// Le bridge encode X-Transcript/X-Answer en %XX (les headers HTTP
// n'autorisent que de l'ISO-8859-1).
static int _hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static String _url_decode(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.length()) {
            int hi = _hex(in[i + 1]), lo = _hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)((hi << 4) | lo); i += 2; }
            else out += c;
        } else {
            out += c;
        }
    }
    return out;
}

// La question peut venir d'un topic MQTT en texte libre.
static String _json_escape(const char* s) {
    String out;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;
        }
    }
    return out;
}

// ---- API LOCALES ----

// À n'appeler QUE depuis loopTask (touche LVGL via _cb).
static void _apply(AiState s, const char* transcript, const char* answer) {
    bool state_changed = (s != _state);
    _state = s;
    if (transcript) {
        strncpy(_transcript, transcript, sizeof(_transcript) - 1);
        _transcript[sizeof(_transcript) - 1] = '\0';
        if (_transcript[0]) log_line("[AI] Question : %s", _transcript);
    }
    if (answer) {
        strncpy(_answer, answer, sizeof(_answer) - 1);
        _answer[sizeof(_answer) - 1] = '\0';
        if (_answer[0]) log_line("[AI] Réponse : %s", _answer);
    }
    if (_cb) _cb(_state);
    // Seul un VRAI changement est republié : sinon chaque chunk de texte
    // republiait le statut en boucle.
    if (state_changed) {
        log_line("[AI] État -> %s", _state_name(s));
        mqtt_publish(TOPIC_AI_STATUS, _state_name(_state));
    }
}

// Depuis _ai_task ou _audio_task : dépose l'état, ai_loop() l'appliquera.
//
// ⚠️ NE PAS revenir à lv_async_call() : c'est une API LVGL comme les autres,
// donc pas thread-safe ici (LV_USE_OS = LV_OS_NONE).
// Cause racine de panics intermittents
static void _post_state_async(AiState s, const char* transcript, const char* answer) {
    if (!_post_queue) return;

    _PostMsg m;
    m.state          = s;
    m.has_transcript = transcript != nullptr;
    m.has_answer     = answer != nullptr;
    m.transcript[0]  = '\0';
    m.answer[0]      = '\0';
    if (transcript) { strncpy(m.transcript, transcript, sizeof(m.transcript) - 1); m.transcript[sizeof(m.transcript) - 1] = '\0'; }
    if (answer)     { strncpy(m.answer, answer, sizeof(m.answer) - 1); m.answer[sizeof(m.answer) - 1] = '\0'; }

    // Sans attente : mieux vaut perdre un rafraîchissement d'affichage que
    // bloquer _audio_task (temps réel) sur une file pleine.
    if (xQueueSend(_post_queue, &m, 0) != pdTRUE) {
        log_line("[AI] File d'etats pleine — mise a jour ignoree");
    }
}

// Lit le PCM de la réponse en PSRAM et le joue. Aucune écriture flash : elle
// suspendait le cache d'instructions et figeait LVGL ~2,9 s après CHAQUE
// réponse (ancien /tts.wav). Le rejeu passe par ai_say().
static void _handle_bridge_response(HTTPClient& http, int http_code, const char* known_transcript) {
    if (http_code != HTTP_CODE_OK) {
        log_line("[AI] Bridge HTTP %d", http_code);
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }

    String transcript = _url_decode(http.header("X-Transcript"));
    String answer     = _url_decode(http.header("X-Answer"));
    if (transcript.length() == 0 && known_transcript) transcript = known_transcript;

    int total = http.getSize();
    size_t buf_capacity = (total > 0) ? (size_t)total : (size_t)PSRAM_STREAM_FALLBACK_BYTES;

    int16_t* pcm_buf = (int16_t*)heap_caps_malloc(buf_capacity, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        log_line("[AI] PSRAM insuffisante pour la réponse TTS");
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }

    NetworkClient* stream = http.getStreamPtr();   // core 3.x : ex-WiFiClient*
    size_t received = 0;
    int remaining = total;

    while (http.connected() && (remaining > 0 || total == -1) && received < buf_capacity) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) break;
            delay(2);
            continue;
        }
        size_t want = min(avail, buf_capacity - received);
        int n = stream->readBytes((uint8_t*)pcm_buf + received, want);
        if (n <= 0) break;
        received += n;
        if (total != -1) remaining -= n;
    }

    _post_state_async(AI_SPEAKING,
                      transcript.length() ? transcript.c_str() : nullptr,
                      answer.length()     ? answer.c_str()     : nullptr);

    audio_play_psram_stream_queue(pcm_buf, received / sizeof(int16_t), true);
}

static void _run_request_audio(const int16_t* pcm_buf, size_t samples) {
    if (!wifi_is_connected()) {
        log_line("[AI] WiFi non connecté, requête audio annulée");
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }
    if (!pcm_buf || samples == 0) {
        log_line("[AI] Buffer audio vide, requête annulée");
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }

    HTTPClient http;
    http.setTimeout(15000);
    http.begin(AI_BRIDGE_URL);
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-Sample-Rate", "16000");
    http.addHeader("X-Samples", String((unsigned)samples));

    int code = http.sendRequest("POST", (uint8_t*)pcm_buf, samples * sizeof(int16_t));
    _handle_bridge_response(http, code, nullptr);
    http.end();
}

static void _run_request_text(const char* question) {
    if (!wifi_is_connected()) {
        log_line("[AI] WiFi non connecté, requête texte annulée");
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }

    HTTPClient http;
    http.setTimeout(15000);
    http.begin(AI_BRIDGE_TEXT_URL);
    http.addHeader("Content-Type", "application/json");

    String body = String("{\"text\":\"") + _json_escape(question) + "\"}";
    int code = http.POST(body);

    _handle_bridge_response(http, code, question);
    http.end();
}

// TTS seul, sans mémorisation côté bridge : pour les invites système, qui ne
// sont pas des échanges.
static void _run_say(const char* text) {
    if (!wifi_is_connected()) {
        log_line("[AI] WiFi non connecté, invite vocale abandonnée");
        _post_state_async(AI_IDLE, nullptr, nullptr);
        return;
    }

    HTTPClient http;
    http.setTimeout(15000);
    http.begin(AI_BRIDGE_SAY_URL);
    http.addHeader("Content-Type", "application/json");

    String body = String("{\"text\":\"") + _json_escape(text) + "\"}";
    int code = http.POST(body);

    _handle_bridge_response(http, code, "");   // chemin commun : SPEAKING puis IDLE
    http.end();
}

// Archivage WAV horodaté côté NAS. ~100 ms de HTTP, contre ~3 s d'écriture
// flash qui gelaient LVGL — et le NAS garde un historique.
static void _run_upload_record(const int16_t* pcm, size_t samples) {
    if (!wifi_is_connected()) {
        log_line("[AI] WiFi non connecté, envoi de la capture annulé");
        return;
    }

    HTTPClient http;
    http.setTimeout(15000);
    http.begin(AI_BRIDGE_RECORD_URL);
    http.addHeader("Content-Type", "application/octet-stream");

    int code = http.POST((uint8_t*)pcm, samples * sizeof(int16_t));
    if (code == HTTP_CODE_OK) {
        log_line("[AI] Capture envoyée au NAS : %s", http.getString().c_str());
    } else {
        log_line("[AI] Envoi capture : HTTP %d", code);
    }
    http.end();
}

static void _ai_task(void* pv) {
    AiCmdMsg msg;
    for (;;) {
        if (xQueueReceive(_ai_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.cmd == AiCmdMsg::CMD_ASK_AUDIO) {
            _post_state_async(AI_THINKING, nullptr, nullptr);
            _run_request_audio(msg.pcm_buf, msg.pcm_samples);
        } else if (msg.cmd == AiCmdMsg::CMD_SAY) {
            _run_say(msg.text);            // pas de THINKING : rien n'est "réfléchi"
        } else if (msg.cmd == AiCmdMsg::CMD_UPLOAD_RECORD) {
            _run_upload_record(msg.pcm_buf, msg.pcm_samples);
        } else {
            _run_request_text(msg.text);   // THINKING déjà posé par ai_on_ask_request
        }

        log_line("[AI] Pile restante (ai_task) : %u octets",
                 (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
}

// Appelé DEPUIS _audio_task, avec le buffer PSRAM d'audio_manager.
static void _on_record_done(const int16_t* pcm_buf, size_t samples, bool cancelled, bool ok,
                            uint16_t speech_ms) {
    // Mémorisé AVANT tout tri : les captures écartées sont celles qu'on veut
    // pouvoir envoyer au NAS pour inspection.
    _last_pcm     = pcm_buf;
    _last_samples = samples;

    if (cancelled) {
        _post_state_async(AI_IDLE, nullptr, nullptr);
        return;
    }
    if (!ok) {
        _post_state_async(AI_ERROR, nullptr, nullptr);
        return;
    }

    // Pas de parole -> on n'envoie RIEN : Whisper hallucine sur autre chose que
    // de la parole, et la fausse question polluerait ensuite l'historique du
    // bridge. Le critère est la DURÉE de parole, jamais le pic d'amplitude.
    if (speech_ms < AUDIO_MIN_SPEECH_MS) {
        log_line("[AI] Aucune parole (%u ms < %d) — aucune requête envoyée",
                 (unsigned)speech_ms, AUDIO_MIN_SPEECH_MS);
        _post_state_async(AI_IDLE, "", "");
        ai_say(AI_NOT_HEARD_TEXT);   // n'empile que : le HTTP part de _ai_task
        return;
    }

    AiCmdMsg msg;
    msg.cmd         = AiCmdMsg::CMD_ASK_AUDIO;
    msg.pcm_buf     = pcm_buf;
    msg.pcm_samples = samples;
    if (_ai_queue) xQueueSend(_ai_queue, &msg, 0);
}

// ---- API PUBLIQUES ----

void ai_init() {
    _ai_queue   = xQueueCreate(2, sizeof(AiCmdMsg));
    _post_queue = xQueueCreate(3, sizeof(_PostMsg));
    if (!_post_queue) log_line("[AI] FATAL: file d'etats non creee");
    // Pile 6144 : à 4096 il ne restait que 1128 o après une requête vocale
    // complète. Ne pas redescendre sans un relevé pris APRÈS un échange complet.
    xTaskCreate(_ai_task, "ai_task", 6144, nullptr, 1, nullptr);
    log_line("[AI] Init OK");
}

// Draine la file d'états. Appelée depuis loop(), seul thread autorisé à
// toucher LVGL. Les états sont cumulatifs : on vide tout d'un coup.
void ai_loop() {
    if (!_post_queue) return;

    _PostMsg m;
    while (xQueueReceive(_post_queue, &m, 0) == pdTRUE) {
        _apply(m.state, m.has_transcript ? m.transcript : nullptr,
                        m.has_answer     ? m.answer     : nullptr);
    }
}

void ai_start_listening() {
    if (_state == AI_LISTENING || _state == AI_THINKING || _state == AI_SPEAKING) return;
    _apply(AI_LISTENING, "", "");
    audio_record_file(AUDIO_RECORD_MAX_SECONDS * 1000, _on_record_done);
}

void ai_stop_listening() {
    if (_state != AI_LISTENING) return;
    audio_stop_recording();     // capture valide -> envoyée normalement
}

void ai_cancel_listening() {
    if (_state != AI_LISTENING) return;
    audio_cancel_recording();   // ramène à idle, rien n'est envoyé
}

void ai_notify_speaking_done() {
    if (_state != AI_SPEAKING) return;
    _apply(AI_IDLE, nullptr, nullptr);
}

// Refait synthétiser la réponse par le bridge plutôt que de relire un WAV
// local : plus rien n'est écrit sur la flash. ai_say() pose l'état lui-même.
void ai_replay_answer() {
    if (_state == AI_LISTENING || _state == AI_THINKING) return;
    if (_answer[0] == '\0') return;
    ai_say(_answer);
}

AiState     ai_get_state()      { return _state; }
const char* ai_get_transcript() { return _transcript; }
const char* ai_get_answer()     { return _answer; }

void ai_set_state_callback(ai_state_cb cb) { _cb = cb; }

// Non bloquant : le HTTP part de _ai_task. Le buffer doit rester valable
// jusque-là — vrai pour le buffer singleton d'audio_manager, tant qu'aucun
// nouvel enregistrement ne démarre.
void ai_upload_pcm(const int16_t* pcm, size_t samples) {
    if (!pcm || samples == 0) {
        log_line("[AI] Aucune capture a envoyer");
        return;
    }
    AiCmdMsg msg;
    msg.cmd         = AiCmdMsg::CMD_UPLOAD_RECORD;
    msg.pcm_buf     = pcm;
    msg.pcm_samples = samples;
    msg.text[0]     = '\0';
    if (_ai_queue) xQueueSend(_ai_queue, &msg, 0);
}

void ai_upload_last_record() {
    ai_upload_pcm(_last_pcm, _last_samples);
}

// Non bloquant : dépose une commande pour _ai_task, d'où le HTTP part
// réellement — appelable depuis n'importe quelle tâche, y compris
// _audio_task dont la pile est étroite.
void ai_say(const char* text) {
    if (!text || !text[0]) return;

    AiCmdMsg msg;
    msg.cmd = AiCmdMsg::CMD_SAY;
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    msg.pcm_buf     = nullptr;
    msg.pcm_samples = 0;
    if (_ai_queue) xQueueSend(_ai_queue, &msg, 0);
}

void ai_on_transcript(const char* text) {
    _apply(_state, text, nullptr);
}

void ai_on_answer(const char* text) {
    _apply(_state, nullptr, text);
}

void ai_on_ask_request(const char* question) {
    if (_state == AI_LISTENING || _state == AI_THINKING || _state == AI_SPEAKING) return;

    uint32_t now = millis();
    if (now - _last_ask_request_ms < ASK_REQUEST_COOLDOWN_MS) {
        log_line("[AI] ai/ask ignoré (trop rapproché — livraison MQTT en double ?)");
        return;
    }
    _last_ask_request_ms = now;

    _apply(AI_THINKING, question, "");

    AiCmdMsg msg;
    msg.cmd = AiCmdMsg::CMD_ASK_TEXT;
    strncpy(msg.text, question, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    msg.pcm_buf     = nullptr;
    msg.pcm_samples = 0;
    if (_ai_queue) xQueueSend(_ai_queue, &msg, 0);
}
