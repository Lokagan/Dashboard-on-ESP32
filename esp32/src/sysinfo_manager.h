#pragma once

// ============================================================
// SYSINFO_MANAGER.H
// Écran de diagnostic système — dessiné dans un TFT_eSprite hors-écran
// puis affiché via un lv_canvas, comme un écran LVGL normal (tap pour
// naviguer/sortir). Non bloquant : display_show_sysinfo() affiche la
// première page et rend la main immédiatement.
// ============================================================

// ---- OBJETS GLOBAUX ----

// Nombre de pages. L'enum interne SiPage s'y aligne par static_assert.
#define SYSINFO_PAGE_COUNT 6

// ---- API PUBLIQUES ----
// Affiche l'écran ; si déjà affiché, passe à la page suivante (bouclage) —
// permet de parcourir les pages depuis MQTT/POST /cmd, sans écran tactile.
void display_show_sysinfo();

// Affiche l'écran directement sur une page (0-based) — les six boutons SysInfo
// du panneau web, qui évitent d'avoir à faire défiler. Hors plage : ignoré et
// journalisé. Si l'écran est déjà affiché, seule la page change.
void display_show_sysinfo_page(int page);

// Journalise l'état mémoire (interne / DMA / PSRAM / fragmentation / pile)
// dans le log circulaire — commande esp32/cmd "mem", relisible via GET /serial.
void sysinfo_log_memory();
