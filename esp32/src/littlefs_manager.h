#pragma once

// ============================================================
// LITTLEFS_MANAGER.H
// Montage LittleFS + accès fichiers génériques, utilisés par
// audio_manager, http_manager, sysinfo_manager et ai_companion.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <LittleFS.h>

// ---- API PUBLIQUES ----
void littlefs_init();          // À appeler dans setup(), AVANT http_init()/audio_init()/ai_companion_init()
bool littlefs_is_mounted();

fs::File littlefs_open(const String& path, const char* mode);
bool     littlefs_exists(const String& path);
bool     littlefs_remove(const String& path);
size_t   littlefs_total_bytes();
size_t   littlefs_used_bytes();
