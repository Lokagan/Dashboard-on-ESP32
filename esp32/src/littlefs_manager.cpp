// ============================================================
// LITTLEFS_MANAGER.CPP — Montage LittleFS + accès fichiers
//
// Point de montage unique. Le reste du firmware passe
// par les wrappers ci-dessous plutôt que d'inclure <LittleFS.h> et
// d'appeler l'objet global directement.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- RESSOURCES LOCALES ----
#include "littlefs_manager.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----
static bool _mounted = false;

// ---- API PUBLIQUES ----

void littlefs_init() {
    // Le nom de partition par défaut de LittleFS.begin() est "spiffs"
    // (historique) — notre partitions.csv nomme la partition "littlefs",
    // donc il faut le préciser explicitement (sinon : partition introuvable).
    _mounted = LittleFS.begin(true, "/littlefs", 10, "littlefs");
    if (!_mounted) {
        log_line("[FS] Montage LittleFS échoué");
        return;
    }
    log_line("[FS] LittleFS : %u/%u octets utilisés",
             (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

bool littlefs_is_mounted() {
    return _mounted;
}

fs::File littlefs_open(const String& path, const char* mode) {
    return LittleFS.open(path, mode);
}

bool littlefs_exists(const String& path) {
    return LittleFS.exists(path);
}

bool littlefs_remove(const String& path) {
    return LittleFS.remove(path);
}

size_t littlefs_total_bytes() {
    return LittleFS.totalBytes();
}

size_t littlefs_used_bytes() {
    return LittleFS.usedBytes();
}
