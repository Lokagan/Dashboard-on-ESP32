#pragma once

// ============================================================
// MQTT_MANAGER.H — connexion au broker et réception des topics.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- API PUBLIQUES ----
void mqtt_init();
void mqtt_loop();           // à appeler dans loop() — keepalive + réception
void mqtt_publish(const char* topic, const char* payload);
bool mqtt_is_connected();

// Commandes "cmd" ou "cmd:arg". Liste de référence dans config.h, sous
// TOPIC_ESP_CMD. Appelée depuis le dispatch MQTT et depuis POST /cmd.
void mqtt_handle_esp_cmd(const char* cmd);
