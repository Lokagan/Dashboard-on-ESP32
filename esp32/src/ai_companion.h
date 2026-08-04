#pragma once

// ============================================================
// AI_COMPANION.H — sprites/avatar de l'écran Companion (ui_ScreenAI)
// ============================================================

// ---- RESSOURCES LOCALES ----
#include "ai_manager.h"

// ---- OBJETS GLOBAUX ----

// Avatar préchargé en PSRAM : chaque frame est un plan RGB565 (2 o/px), sans
// plan alpha — les frames sont des carrés opaques. Consommateur externe : la
// page MEMOIRE de SysInfo (_si_alloc). Le nombre de frames est vérifié par
// static_assert contre la vraie liste, dans ai_companion.cpp.
#define COMPANION_FRAME_W     120
#define COMPANION_FRAME_H     120
#define COMPANION_FRAME_COUNT 42
#define COMPANION_FRAME_SZ    (COMPANION_FRAME_W * COMPANION_FRAME_H * 2)
#define COMPANION_PSRAM_BYTES (COMPANION_FRAME_COUNT * COMPANION_FRAME_SZ)

// ---- API PUBLIQUES ----
void ai_companion_init();                  // une fois, après ui_init()
void ai_companion_set_state(AiState state); // appelé depuis _ai_state_cb
void ai_companion_pause();
void ai_companion_resume();
