// ============================================================
// MQTT_MANAGER.CPP — client esp-mqtt : tâche dédiée (réception/parsing hors
// loopTask), marshalling des messages par file FreeRTOS vers loopTask où
// LVGL est appelé, et traitement de esp32/cmd.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "mqtt_manager.h"
#include "display_manager.h"
#include "audio_manager.h"
#include "ai_manager.h"
#include "sysinfo_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
static esp_mqtt_client_handle_t _client    = nullptr;
static volatile bool            _connected = false;

// File mqtt_task → loopTask : la réception se fait dans la tâche esp-mqtt, mais
// le dispatch appelle display_update_* (donc LVGL, NON thread-safe ici) : il
// DOIT s'exécuter sur loopTask. On y passe des copies PSRAM (topic + payload).
typedef struct { char* topic; char* payload; } MqttMsg;
static QueueHandle_t _rx_queue = nullptr;

// Réassemblage d'un payload fragmenté (staging PSRAM, zéro interne).
static char*  _payload_buf = nullptr;
static char   _rx_topic[64];
static size_t _rx_len = 0;

// Aides des messages d'erreur de esp32/cmd.
// ⚠️ Doivent tenir dans LOG_LINE_LEN (100 OCTETS, dont 10 d'horodatage)
static const char* CMD_HELP      = "page:X, brightness, volume, loop:on|off, mem, saverec, spy, tree, shot, reboot";
static const char* CMD_HELP_PAGE = "home, nas, freebox, ai, sysinfo[1-6], disks, downloads, connections, devices, activity";

// ---- API LOCALES ----

// Abonnements : rejoués à chaque MQTT_EVENT_CONNECTED (donc aussi à la
// reconnexion auto d'esp-mqtt), ce qui remplace l'ancien _mqtt_reconnect.
static void _subscribe_all() {
    // --- NAS ---
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_CPU, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_RAM, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_TEMP, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_NET_RX, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_NET_TX, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_VOL1_USED_PCT, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_VOL1_STATUS, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_VOL1_READ_MBS, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_VOL1_WRITE_MBS, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_DISKS, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_DOWNLOADS, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_NAS_CONNECTIONS, 0);

    // --- Freebox ---
    esp_mqtt_client_subscribe(_client, TOPIC_FB_RATE_DOWN, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_RATE_UP, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_BANDWIDTH_DOWN, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_BANDWIDTH_UP, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_STATE, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_IPV4, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_DEVICES_ACTIVE, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_DEVICES_TOTAL, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_FB_DEVICES, 0);

    // --- IA ---
    // ⚠️ PAS d'abonnement à TOPIC_AI_STATUS : l'ESP32 en est la source de
    // vérité et le publie lui-même (sinon oscillation sur son propre écho).
    esp_mqtt_client_subscribe(_client, TOPIC_AI_TRANSCRIPT, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_AI_ANSWER, 0);
    esp_mqtt_client_subscribe(_client, TOPIC_AI_ASK, 0);

    // --- ESP32 ---
    esp_mqtt_client_subscribe(_client, TOPIC_ESP_CMD, 0);
}

// Empile une copie PSRAM du message pour drainage sur loopTask. File pleine ou
// PSRAM KO : message abandonné (une métrique ratée se re-publie en 5 s).
static void _rx_enqueue(const char* topic, const char* payload, size_t plen) {
    size_t tlen = strlen(topic);
    char* t = (char*)heap_caps_malloc(tlen + 1, MALLOC_CAP_SPIRAM);
    char* p = (char*)heap_caps_malloc(plen + 1, MALLOC_CAP_SPIRAM);
    if (!t || !p) { heap_caps_free(t); heap_caps_free(p); return; }
    memcpy(t, topic, tlen + 1);
    memcpy(p, payload, plen); p[plen] = '\0';
    MqttMsg m = { t, p };
    if (xQueueSend(_rx_queue, &m, 0) != pdTRUE) { heap_caps_free(t); heap_caps_free(p); }
}

// Dispatch — s'exécute sur loopTask (drainé par mqtt_loop). Recopie sans
// ré-alloc : un String dupliquerait en interne.
static void _dispatch(const char* topic, const char* value) {
    // --- NAS ---
    if      (strcmp(topic, TOPIC_NAS_CPU) == 0)             display_update_nas_cpu(atoi(value));
    else if (strcmp(topic, TOPIC_NAS_RAM) == 0)             display_update_nas_ram(atoi(value));
    else if (strcmp(topic, TOPIC_NAS_TEMP) == 0)            display_update_nas_temp(atof(value));
    else if (strcmp(topic, TOPIC_NAS_NET_RX) == 0)          display_update_nas_net_in(atoi(value));
    else if (strcmp(topic, TOPIC_NAS_NET_TX) == 0)          display_update_nas_net_out(atoi(value));
    else if (strcmp(topic, TOPIC_NAS_VOL1_USED_PCT) == 0)   display_update_nas_vol_pct(atoi(value));
    else if (strcmp(topic, TOPIC_NAS_VOL1_STATUS) == 0)     display_update_nas_vol_status(value);
    else if (strcmp(topic, TOPIC_NAS_VOL1_READ_MBS) == 0)   display_update_nas_vol_read(atof(value));
    else if (strcmp(topic, TOPIC_NAS_VOL1_WRITE_MBS) == 0)  display_update_nas_vol_write(atof(value));
    else if (strcmp(topic, TOPIC_NAS_DISKS) == 0)           display_update_nas_disks(value);
    else if (strcmp(topic, TOPIC_NAS_DOWNLOADS) == 0)       display_update_nas_downloads(value);
    else if (strcmp(topic, TOPIC_NAS_CONNECTIONS) == 0)     display_update_nas_connections(value);

    // --- Freebox ---
    else if (strcmp(topic, TOPIC_FB_RATE_DOWN) == 0)        display_update_fb_down(atof(value));
    else if (strcmp(topic, TOPIC_FB_RATE_UP) == 0)          display_update_fb_up(atof(value));
    else if (strcmp(topic, TOPIC_FB_BANDWIDTH_DOWN) == 0)   display_update_fb_bw_down(atoi(value));
    else if (strcmp(topic, TOPIC_FB_BANDWIDTH_UP) == 0)     display_update_fb_bw_up(atoi(value));
    else if (strcmp(topic, TOPIC_FB_STATE) == 0)            display_update_fb_state(value);
    else if (strcmp(topic, TOPIC_FB_IPV4) == 0)             display_update_fb_ipv4(value);
    else if (strcmp(topic, TOPIC_FB_DEVICES_ACTIVE) == 0)   display_update_fb_devices_active(atoi(value));
    else if (strcmp(topic, TOPIC_FB_DEVICES_TOTAL) == 0)    display_update_fb_devices_total(atoi(value));
    else if (strcmp(topic, TOPIC_FB_DEVICES) == 0)          display_update_fb_devices(value);

    // --- IA ---
    else if (strcmp(topic, TOPIC_AI_TRANSCRIPT) == 0)       ai_on_transcript(value);
    else if (strcmp(topic, TOPIC_AI_ANSWER) == 0)           ai_on_answer(value);
    else if (strcmp(topic, TOPIC_AI_ASK) == 0)              ai_on_ask_request(value);

    // --- ESP32 ---
    else if (strcmp(topic, TOPIC_ESP_CMD) == 0) {
        log_line("[MQTT] Commande reçue : %s", value);
        mqtt_handle_esp_cmd(value);
    }
}

// Handler esp-mqtt — tâche mqtt_task, PAS loopTask : aucun appel LVGL ici, tout
// passe par la file. topic/data sont délimités par longueur (non terminés \0).
static void _mqtt_event_handler(void*, esp_event_base_t, int32_t id, void* data) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            _connected = true;
            log_line("[MQTT] Connecté !");
            _subscribe_all();
            esp_mqtt_client_publish(_client, TOPIC_ESP_STATUS, "online", 0, 0, 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            _connected = false;
            log_line("[MQTT] Déconnecté - reconnexion auto");
            break;

        case MQTT_EVENT_DATA: {
            if (!_payload_buf) break;
            // Premier fragment : capture le topic (absent des suivants) et RAZ.
            if (e->current_data_offset == 0) {
                size_t tl = (size_t)e->topic_len < sizeof(_rx_topic) - 1
                            ? (size_t)e->topic_len : sizeof(_rx_topic) - 1;
                memcpy(_rx_topic, e->topic, tl);
                _rx_topic[tl] = '\0';
                _rx_len = 0;
            }
            // Accumule (clamp à MQTT_BUFFER_SIZE, comme l'ancien code).
            size_t space = MQTT_BUFFER_SIZE - 1 - _rx_len;
            size_t n = (size_t)e->data_len < space ? (size_t)e->data_len : space;
            memcpy(_payload_buf + _rx_len, e->data, n);
            _rx_len += n;
            // Dernier fragment : termine et empile. Un payload tronqué donne un
            // JSON invalide, donc une table vide : sans cette ligne (UNE par
            // message, pas une par fragment) la panne n'a aucune trace.
            if (e->current_data_offset + e->data_len >= e->total_data_len) {
                if ((size_t)e->total_data_len > _rx_len)
                    log_line("[MQTT] %s TRONQUE : %d o recus sur %d, augmenter MQTT_BUFFER_SIZE",
                             _rx_topic, (int)_rx_len, e->total_data_len);
                _payload_buf[_rx_len] = '\0';
                _rx_enqueue(_rx_topic, _payload_buf, _rx_len);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            log_line("[MQTT] Erreur (type=%d)", e->error_handle ? e->error_handle->error_type : -1);
            break;

        default:
            break;
    }
}

// ---- API PUBLIQUES ----

void mqtt_handle_esp_cmd(const char* cmd) {
    char buf[32];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* sep = strchr(buf, ':');
    const char* arg = "";
    if (sep) {
        *sep = '\0';
        arg = sep + 1;
    }

    if (strcmp(buf, "page") == 0) {
        if      (strcmp(arg, "home")        == 0) display_show_home();
        else if (strcmp(arg, "nas")         == 0) display_show_nas();
        else if (strcmp(arg, "freebox")     == 0) display_show_freebox();
        else if (strcmp(arg, "ai")          == 0) display_show_ai();
        // "sysinfo" seul = page suivante (bouclage) ; "sysinfo1".."sysinfo6" =
        // accès direct, pour les six boutons SysInfo du panneau web.
        else if (strncmp(arg, "sysinfo", 7) == 0) {
            const char* p = arg + 7;
            if (*p == '\0')                                   display_show_sysinfo();
            else if (p[0] >= '1' && p[0] <= '9' && p[1] == '\0') display_show_sysinfo_page(p[0] - '1');
            else {
                log_line("[MQTT] page inconnue : %s", arg);
                log_line("[MQTT] pages : %s", CMD_HELP_PAGE);
                return;
            }
        }
        else if (strcmp(arg, "disks")       == 0) display_show_TABLE_NAS_DISKS(nullptr);
        else if (strcmp(arg, "downloads")   == 0) display_show_TABLE_NAS_DOWNLOADS(nullptr);
        else if (strcmp(arg, "connections") == 0) display_show_TABLE_NAS_CONNECTIONS(nullptr);
        else if (strcmp(arg, "devices")     == 0) display_show_TABLE_FBX_DEVICES(nullptr);
        else if (strcmp(arg, "activity")    == 0) display_show_TABLE_FBX_ACTIVITY(nullptr);
        else {
            log_line("[MQTT] page inconnue : %s", arg);
            log_line("[MQTT] pages : %s", CMD_HELP_PAGE);
            return;
        }
        log_line("[MQTT] Page → %s", arg);

    } else if (strcmp(buf, "brightness") == 0) {
        int val = atoi(arg);
        if (val < 1 || val > 100) {
            log_line("[MQTT] Luminosité hors plage (1-100) : %s", arg);
        } else {
            display_set_brightness(val);
            log_line("[MQTT] Luminosité → %d%%", val);
        }

    } else if (strcmp(buf, "volume") == 0) {
        int val = atoi(arg);
        if (val < 0 || val > 100) {
            log_line("[MQTT] Volume hors plage (0-100) : %s", arg);
        } else {
            audio_set_volume(val);
            display_sync_volume_slider(val);
            log_line("[MQTT] Volume → %d%%", val);
        }

    } else if (strcmp(buf, "loop") == 0) {
        if      (strcmp(arg, "on")  == 0) log_set_loop_measure(true);
        else if (strcmp(arg, "off") == 0) log_set_loop_measure(false);
        else log_line("[MQTT] loop attend on|off : %s", arg);

    } else if (strcmp(buf, "mem") == 0) {
        sysinfo_log_memory();

    } else if (strcmp(buf, "saverec") == 0) {
        ai_upload_last_record();

    } else if (strcmp(buf, "spy") == 0) {
        // log_clear d'abord : 40 lignes seulement, le résultat serait évacué
        // par les "slow frame" avant d'avoir pu être copié.
        log_clear();
        display_spy_invalidations(24);

    } else if (strcmp(buf, "tree") == 0) {
        log_clear();                     // même raison que "spy"
        display_dump_tree();

    } else if (strcmp(buf, "shot") == 0) {
        if (display_capture_screen()) log_line("[MQTT] Capture armée — GET /screen.bmp");
        else                          log_line("[MQTT] Capture KO (PSRAM)");

    } else if (strcmp(buf, "reboot") == 0) {
        log_line("[MQTT] Redémarrage...");
        delay(100);   // laisse partir la ligne de journal
        ESP.restart();

    } else {
        log_line("[MQTT] cmd inconnue : %s", cmd);
        log_line("[MQTT] cmds : %s", CMD_HELP);
    }
}

void mqtt_init() {
    _payload_buf = (char*)heap_caps_malloc(MQTT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!_payload_buf) log_line("[MQTT] FATAL: PSRAM KO pour le buffer de réassemblage");

    _rx_queue = xQueueCreate(MQTT_RX_QUEUE_LEN, sizeof(MqttMsg));
    if (!_rx_queue) log_line("[MQTT] FATAL: file de réception KO");

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri           = MQTT_URI;
    cfg.credentials.client_id        = MQTT_CLIENT_ID;
    cfg.session.keepalive            = MQTT_KEEPALIVE_S;
    cfg.session.last_will.topic      = TOPIC_ESP_STATUS;   // LWT : "offline" sur coupure brutale
    cfg.session.last_will.msg        = "offline";
    cfg.session.last_will.qos        = 0;
    cfg.session.last_will.retain     = 0;
    cfg.buffer.size                  = MQTT_BUFFER_SIZE;
    cfg.buffer.out_size              = MQTT_OUT_BUFFER_SIZE;
    cfg.task.stack_size              = STACK_BYTES_MQTT_TASK;   // sous le défaut 6144
    cfg.network.reconnect_timeout_ms = MQTT_RECONNECT_MS;

    _client = esp_mqtt_client_init(&cfg);
    if (!_client) { log_line("[MQTT] FATAL: init client KO"); return; }
    esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, _mqtt_event_handler, nullptr);
    esp_mqtt_client_start(_client);
    log_line("[MQTT] Client démarré (%s)", MQTT_URI);
}

// Drainé sur loopTask : c'est ICI que LVGL est touché, jamais dans le handler.
void mqtt_loop() {
    if (!_rx_queue) return;
    MqttMsg m;
    while (xQueueReceive(_rx_queue, &m, 0) == pdTRUE) {
        _dispatch(m.topic, m.payload);
        heap_caps_free(m.topic);
        heap_caps_free(m.payload);
    }
}

void mqtt_publish(const char* topic, const char* payload) {
    if (_connected) esp_mqtt_client_publish(_client, topic, payload, 0, 0, 0);
}

bool mqtt_is_connected() {
    return _connected;
}

// Coupe le client le temps d'un flash OTA : le broker saturerait sinon l'airtime
// 2,4 GHz avec nas/#/freebox/#. onError relance ; le succès reboote.
void mqtt_ota_suspend() { if (_client) esp_mqtt_client_stop(_client); }
void mqtt_ota_resume()  { if (_client) esp_mqtt_client_start(_client); }
