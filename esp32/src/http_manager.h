#pragma once

// ============================================================
// HTTP_MANAGER.H — gestionnaire de fichiers LittleFS, commandes ESP32
// (POST /cmd), question IA (POST /ai_ask, GET /ai_status) et invite
// vocale (POST /say). Tourne sur une tâche dédiée, cf. .cpp
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- RESSOURCES LOCALES ----
#include "littlefs_manager.h"
#include "mqtt_manager.h"
#include "log_manager.h"
#include "ai_manager.h"

// ---- API PUBLIQUES ----
void http_init();   // à appeler APRÈS littlefs_init()
void http_loop();   // exécute une commande en attente — tout le reste est sur _http_task
