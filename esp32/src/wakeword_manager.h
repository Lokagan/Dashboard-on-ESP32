#pragma once

// ============================================================
// WAKEWORD_MANAGER.H — Détection du mot-clé "Jarvis" (ESP_SR / WakeNet)
// Utilise le wrapper Arduino officiel ESP_SR (core 3.x) : la détection tourne
// dans la tâche interne d'ESP_SR (réglée par Espressif), qui lit le micro sur
// le bus I2S partagé fourni par audio_manager (audio_get_i2s()). Sur détection,
// déclenche l'assistant vocal exactement comme le bouton tactile.
// ============================================================

// ---- API PUBLIQUES ----

// À appeler APRÈS audio_init() (a besoin du bus I2S déjà démarré) : lance ESP_SR
// en mode wake word sur le modèle "jarvis" (partition flash "model").
void wakeword_init();

// À appeler depuis loop() : traite une détection en attente. Le callback d'ESP_SR
// s'exécute dans SA tâche interne et ne peut donc rien faire d'autre que lever un
// drapeau — tout le déclenchement (LVGL, état IA) a lieu ici, sur loopTask.
void wakeword_loop();

// Chorégraphie d'accès exclusif au bus I2S : audio_manager appelle pause avant
// toute opération audio (enregistrement/lecture) et resume après, pour qu'ESP_SR
// (qui lit le micro en continu) et audio_manager ne lisent jamais en même temps.
// Sûrs à appeler même avant wakeword_init() (no-op tant qu'ESP_SR n'a pas démarré).
void wakeword_pause();
void wakeword_resume();

// Interne consommé par ESP_SR, mesuré au boot (delta libre avant/après begin) —
// pour le bilan mémoire de sysinfo_manager. 0 si ESP_SR n'a pas (encore) démarré.
uint32_t wakeword_esp_sr_internal_bytes();
