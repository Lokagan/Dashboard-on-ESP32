#pragma once

// ============================================================
// DISPLAY_DRIVER.H — dalle ILI9341 par esp_lcd (ESP-IDF), SEUL propriétaire du bus SPI.
//
// Le transfert est ASYNCHRONE : panel_flush() rend la main dès la mise en
// file, la fin réelle est signalée par le callback passé à panel_init().
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- API PUBLIQUES ----

// done_cb est appelé DEPUIS L'ISR de fin de transfert : y faire le strict
// minimum (poser un drapeau, lv_display_flush_ready). Jamais de LVGL au-delà.
typedef void (*panel_done_cb_t)(void);

// max_transfer_bytes dimensionne la chaîne de descripteurs DMA, prise en RAM
// INTERNE. ⚠️ Doit couvrir le plus gros panel_flush() d'un seul tenant (bande
// LVGL ou bande de rebond OTA) : en dessous, le transfert échoue. Un plein
// écran y suffirait mais coûterait ~0,8 Ko d'interne pour rien.
bool panel_init(panel_done_cb_t done_cb, size_t max_transfer_bytes);

// Pousse un rectangle. Retourne immédiatement : les pixels sont encore lus par
// le DMA au retour, ne pas réécrire le buffer avant le callback.
void panel_flush(int x1, int y1, int x2, int y2, const void* px);

// Vrai tant qu'un transfert est en vol. Pour une attente COURTE (une bande),
// tourner dessus coûte moins cher qu'un vTaskDelay (tick de 1 ms).
bool panel_busy(void);

// Attend la fin des transferts en cours. À utiliser avant toute écriture
// concurrente sur le buffer, et avant de céder le bus.
void panel_wait(void);

// ---- CAPTURE D'ÉCRAN ----
// panel_flush() est le goulot unique de tout ce qui atteint la dalle : la
// capture s'y branche et voit donc TOUT. Armer, laisser passer une frame,
// lire le buffer. Buffer PSRAM alloué au premier armement, jamais rendu.
// ⚠️ La fin de capture est décidée par l'APPELANT (panel_capture_done) : ici on
// ne sait pas distinguer les bandes du repeint demandé de celles d'une frame
// voisine. display_capture_screen() encadre un lv_refr_now() synchrone.
// ⚠️ Attendre panel_capture_ready() ne suffit PAS : il est resté vrai depuis la
// capture précédente. Relever panel_capture_seq() AVANT de demander, puis
// attendre qu'il change — sinon on sert l'image d'avant.
bool            panel_capture_arm(void);      // false si PSRAM indisponible
void            panel_capture_done(void);     // la frame est complète : fige le buffer
bool            panel_capture_ready(void);    // vrai quand le buffer est figé et rempli
uint32_t        panel_capture_seq(void);      // incrémenté à chaque capture terminée
bool            panel_capture_allocated(void);// buffer déjà pris ? (page MÉMOIRE de SysInfo)
const uint16_t* panel_capture_buffer(void);   // RGB565 ordre dalle, nullptr si vide

// Fréquence réellement appliquée par le contrôleur SPI, en Hz.
// ⚠️ Elle diffère de la valeur demandée : l'horloge est un diviseur ENTIER de l'APB (80 MHz),
// donc 80/40/26,7/20 MHz et rien entre.
uint32_t panel_actual_hz(void);
