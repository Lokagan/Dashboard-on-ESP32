#pragma once

// ============================================================
// DISPLAY_MANAGER.H — LVGL 9.x sur esp_lcd (ILI9341, cf. display_driver.h)
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <lvgl.h>

// ---- RESSOURCES LOCALES ----
#include "config.h"

// ---- OBJETS GLOBAUX ----

// Buffers de dessin LVGL, en RAM INTERNE Format RGB565_SWAPPED : 2 octets/pixel
// LV_BUF_N = 1 ou 2 (simple ou double buffer asynchrones)
// Meilleures valeurs possibles : 2 buffers de 16 lignes
#define LV_BUF_N 2
#define LV_BUF_LINES 16
#define LV_BUF_BYTES (SCREEN_WIDTH * LV_BUF_LINES * 2)

// Buffers JSON des tables, alloués en PSRAM. Restent nullptr si l'allocation
// échoue → tous les accès sont gardés. Second consommateur : _si_alloc.
#define JSON_NAS_DISKS_SIZE        1024
#define JSON_NAS_DOWNLOADS_SIZE    2048
#define JSON_NAS_CONNECTIONS_SIZE  4096
#define JSON_FBX_DEVICES_SIZE      4096
#define JSON_TOTAL_SIZE (JSON_NAS_DISKS_SIZE + JSON_NAS_DOWNLOADS_SIZE + \
                         JSON_NAS_CONNECTIONS_SIZE + JSON_FBX_DEVICES_SIZE)

// ---- API PUBLIQUES ----

// --- Init / Loop ---
void display_init();
void display_loop();

// Coupe/reprend LVGL. À utiliser autour de toute écriture directe sur la dalle
// depuis une autre tâche (OTA) : même bus SPI, sans mutex.
void display_pause();
void display_resume();

// --- Navigation ---
void display_show_home();
void display_show_nas();
void display_show_freebox();
void display_show_ai();
void display_show_ai_auto();   // vocal uniquement : arme le retour auto à l'écran précédent

// --- Table générique — extern "C" pour correspondre à ui_events.h (SquareLine) ---
#ifdef __cplusplus
extern "C" {
#endif

void display_show_TABLE_NAS_DISKS(lv_event_t* e);
void display_show_TABLE_NAS_DOWNLOADS(lv_event_t* e);
void display_show_TABLE_NAS_CONNECTIONS(lv_event_t* e);
void display_show_TABLE_FBX_DEVICES(lv_event_t* e);
void display_show_TABLE_FBX_ACTIVITY(lv_event_t* e);

#ifdef __cplusplus
}
#endif

// --- NAS ---
void display_update_nas_cpu(int percent);
void display_update_nas_ram(int percent);
void display_update_nas_temp(float celsius);
void display_update_nas_net_in(int kbps);
void display_update_nas_net_out(int kbps);
void display_update_nas_vol_pct(int percent);
void display_update_nas_vol_status(const char* status);
void display_update_nas_vol_read(float mbs);
void display_update_nas_vol_write(float mbs);
void display_update_nas_disks(const char* json);
void display_update_nas_downloads(const char* json);
void display_update_nas_connections(const char* json);

// --- Freebox ---
void display_update_fb_down(float mbps);
void display_update_fb_up(float mbps);
void display_update_fb_bw_down(int mbps);
void display_update_fb_bw_up(int mbps);
void display_update_fb_state(const char* state);
void display_update_fb_ipv4(const char* ip);
void display_update_fb_devices_active(int count);
void display_update_fb_devices_total(int count);
void display_update_fb_devices(const char* json);

// --- Utils ---
// Diagnostic (cmd "spy") — journalise les `count` prochaines invalidations
// LVGL, rectangles AVANT fusion, puis se désarme.
void display_spy_invalidations(uint8_t count);
// Diagnostic (cmd "tree") — arbre des widgets de l'écran actif. À croiser avec "spy".
void display_dump_tree();
// Diagnostic (cmd "shot") — arme la capture et force un repeint plein écran.
bool display_capture_screen();
void display_set_brightness(int percent);
void display_sync_volume_slider(int percent);
