// ============================================================
// MQTT_MANAGER.CPP — client PubSubClient : connexion, abonnements,
// dispatch des messages vers display_manager / ai_manager, et
// traitement de esp32/cmd.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "display_manager.h"
#include "audio_manager.h"
#include "ai_manager.h"
#include "sysinfo_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
static WiFiClient    _wifi_client;
static PubSubClient  _mqtt(_wifi_client);
static unsigned long _last_reconnect_attempt = 0;

// Tampon de réception des payloads, en PSRAM (4 Ko d'interne rendus).
static char* _payload_buf = nullptr;

// Aides des messages d'erreur de esp32/cmd.
// ⚠️ Doivent tenir dans LOG_LINE_LEN (100 OCTETS, dont 10 d'horodatage)
static const char* CMD_HELP      = "page:X, brightness, volume, loop:on|off, mem, saverec, spy, tree, reboot";
static const char* CMD_HELP_PAGE = "home, nas, freebox, ai, sysinfo[1-6], disks, downloads, connections, devices, activity";

// ---- SÉQUENCE D'INITIALISATION ----

// (Re)connexion — appelée depuis mqtt_init() et depuis mqtt_loop() tant que la
// connexion n'est pas établie.
static void _mqtt_reconnect() {
    if (!wifi_is_connected()) return;

    log_line("[MQTT] Connexion à %s:%d en cours...", MQTT_BROKER, MQTT_PORT);

    if (!_mqtt.connect(MQTT_CLIENT_ID)) {
        log_line("[MQTT] Echec, rc=%d - retry dans %ds", _mqtt.state(), MQTT_RECONNECT_MS / 1000);
        return;
    }
    log_line("[MQTT] Connecté !");

    // --- NAS ---
    _mqtt.subscribe(TOPIC_NAS_CPU);
    _mqtt.subscribe(TOPIC_NAS_RAM);
    _mqtt.subscribe(TOPIC_NAS_TEMP);
    _mqtt.subscribe(TOPIC_NAS_NET_RX);
    _mqtt.subscribe(TOPIC_NAS_NET_TX);
    _mqtt.subscribe(TOPIC_NAS_VOL1_USED_PCT);
    _mqtt.subscribe(TOPIC_NAS_VOL1_STATUS);
    _mqtt.subscribe(TOPIC_NAS_VOL1_READ_MBS);
    _mqtt.subscribe(TOPIC_NAS_VOL1_WRITE_MBS);
    _mqtt.subscribe(TOPIC_NAS_DISKS);
    _mqtt.subscribe(TOPIC_NAS_DOWNLOADS);
    _mqtt.subscribe(TOPIC_NAS_CONNECTIONS);

    // --- Freebox ---
    _mqtt.subscribe(TOPIC_FB_RATE_DOWN);
    _mqtt.subscribe(TOPIC_FB_RATE_UP);
    _mqtt.subscribe(TOPIC_FB_BANDWIDTH_DOWN);
    _mqtt.subscribe(TOPIC_FB_BANDWIDTH_UP);
    _mqtt.subscribe(TOPIC_FB_STATE);
    _mqtt.subscribe(TOPIC_FB_IPV4);
    _mqtt.subscribe(TOPIC_FB_DEVICES_ACTIVE);
    _mqtt.subscribe(TOPIC_FB_DEVICES_TOTAL);
    _mqtt.subscribe(TOPIC_FB_DEVICES);

    // --- IA ---
    // ⚠️ PAS d'abonnement à TOPIC_AI_STATUS : l'ESP32 en est la source de
    // vérité et le publie lui-même. S'y abonner le faisait s'appliquer son
    // propre écho : oscillation permanente.
    // Le seul état publié par le bridge est "error", que le firmware détecte déjà via le code HTTP.
    _mqtt.subscribe(TOPIC_AI_TRANSCRIPT);
    _mqtt.subscribe(TOPIC_AI_ANSWER);
    _mqtt.subscribe(TOPIC_AI_ASK);

    // --- ESP32 ---
    _mqtt.subscribe(TOPIC_ESP_CMD);

    _mqtt.publish(TOPIC_ESP_STATUS, "online");
}

// ---- API LOCALES ----

// Recopie en une fois plutôt que caractère par caractère : évite les
// réallocations heap sur les gros payloads, et un String dupliquerait le
// payload en RAM interne.
static void _mqtt_on_message(char* topic, byte* payload, unsigned int length) {
    if (!_payload_buf) return;
    size_t len = length < MQTT_BUFFER_SIZE - 1 ? length : MQTT_BUFFER_SIZE - 1;
    memcpy(_payload_buf, payload, len);
    _payload_buf[len] = '\0';
    const char* value = _payload_buf;

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
        // log_clear d'abord : le journal ne fait que 40 lignes et les "slow
        // frame" en ajoutent ~5/s — sans ça le résultat est évacué avant
        // d'avoir pu être copié.
        log_clear();
        display_spy_invalidations(24);

    } else if (strcmp(buf, "tree") == 0) {
        log_clear();                     // même raison que "spy"
        display_dump_tree();

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
    if (!_payload_buf) log_line("[MQTT] FATAL: PSRAM KO pour le buffer de payload");

    _mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    _mqtt.setCallback(_mqtt_on_message);
    _mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    _mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    _mqtt_reconnect();
}

void mqtt_loop() {
    if (!_mqtt.connected()) {
        unsigned long now = millis();
        if (now - _last_reconnect_attempt > MQTT_RECONNECT_MS) {
            _last_reconnect_attempt = now;
            _mqtt_reconnect();
        }
    }
    _mqtt.loop();
}

void mqtt_publish(const char* topic, const char* payload) {
    if (_mqtt.connected()) _mqtt.publish(topic, payload);
}

bool mqtt_is_connected() {
    return _mqtt.connected();
}
