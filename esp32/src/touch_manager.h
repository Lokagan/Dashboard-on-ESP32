#pragma once

// ============================================================
// TOUCH_MANAGER.H
// Gestion du tactile FT6336G (I2C)
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- API PUBLIQUES ----
void touch_init();      // Init I2C + FT6336G
void touch_loop();      // À appeler dans loop() — lecture des événements
