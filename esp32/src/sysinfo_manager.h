#pragma once

// ============================================================
// SYSINFO_MANAGER.H — Écran de diagnostic système
// dessiné par gfx dans un buffer hors-écran puis affiché via un lv_canvas,
// comme un écran LVGL normal (tap pour naviguer/sortir). Non bloquant.
// ============================================================

// ---- OBJETS GLOBAUX ----

// Nombre de pages. La table interne _pages[] s'y aligne par static_assert.
#define SYSINFO_PAGE_COUNT 6

// ---- API PUBLIQUES ----
// Affiche l'écran ; si déjà affiché, passe à la page suivante (bouclage).
void display_show_sysinfo();

// Affiche l'écran directement sur une page (0-based). Hors plage : ignoré et
// journalisé. Si l'écran est déjà affiché, seule la page change.
void display_show_sysinfo_page(int page);

// Journalise l'état mémoire (interne / DMA / PSRAM / fragmentation / pile)
// dans le log circulaire — commande esp32/cmd "mem", relisible via GET /serial.
void sysinfo_log_memory();
