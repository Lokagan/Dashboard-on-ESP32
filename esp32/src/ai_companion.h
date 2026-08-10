#pragma once

// ============================================================
// AI_COMPANION.H — sprites/avatar de l'écran Companion (ui_ScreenAI)
// ============================================================

// ---- RESSOURCES LOCALES ----
#include "ai_manager.h"

// ---- OBJETS GLOBAUX ----

// Avatar préchargé en PSRAM : chaque frame est un plan RGB565 (2 o/px), sans
// plan alpha — les frames sont des carrés opaques.
// ⚠️ Le nombre de frames n'est PAS déclaré ici : il est découvert sur LittleFS
// au boot, animation par animation. Ce plafond ne borne que le sondage.
#define COMPANION_FRAME_W     120
#define COMPANION_FRAME_H     120
#define COMPANION_FRAME_MAX   64
#define COMPANION_FRAME_SZ    (COMPANION_FRAME_W * COMPANION_FRAME_H * 2)

// ---- API PUBLIQUES ----
void ai_companion_init();                  // une fois, après ui_init()
void ai_companion_set_state(AiState state); // appelé depuis _ai_state_cb
void ai_companion_pause();
void ai_companion_resume();

// Empreinte PSRAM réelle, une fois les frames découvertes. Consommateur
// externe : la page MEMOIRE de SysInfo (inv::allocs).
uint32_t ai_companion_psram_bytes();
