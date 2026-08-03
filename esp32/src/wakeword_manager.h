#pragma once

// ============================================================
// WAKEWORD_MANAGER.H — Détection du mot-clé "Jarvis" (ESP_SR / WakeNet)
// La détection tourne dans la tâche interne d'ESP_SR, qui lit le micro sur le
// bus I2S partagé fourni par audio_manager. Sur détection, déclenche
// l'assistant vocal exactement comme le bouton tactile.
// ============================================================

// ---- API PUBLIQUES ----

// À appeler APRÈS audio_init() (a besoin du bus I2S démarré) : lance ESP_SR en
// mode wake word sur le modèle "jarvis" (partition flash "model").
void wakeword_init();

// À appeler depuis loop() : traite une détection en attente. Le callback d'ESP_SR
// ne peut que lever un drapeau — tout le déclenchement a lieu ici, sur loopTask.
void wakeword_loop();

// Accès exclusif au bus I2S : audio_manager encadre par pause/resume toute
// opération audio, pour qu'ESP_SR et lui ne lisent jamais le micro en même temps.
// Sûrs à appeler avant wakeword_init() (no-op tant qu'ESP_SR n'a pas démarré).
void wakeword_pause();
void wakeword_resume();

// Coupe ESP_SR le temps d'un flash OTA (il volerait sinon le cœur 0 à l'ota_task).
// Verrou DUR : tient malgré les wakeword_resume() émis par _audio_task après un son.
void wakeword_ota_suspend();
void wakeword_ota_resume();

// Interne consommé par ESP_SR, mesuré au boot. 0 s'il n'a pas (encore) démarré.
uint32_t wakeword_esp_sr_internal_bytes();
