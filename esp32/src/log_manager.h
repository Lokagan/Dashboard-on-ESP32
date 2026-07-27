#pragma once

// ============================================================
// LOG_MANAGER.H — Journal circulaire des logs, survivant à un reset logiciel/crash
// (RTC_NOINIT_ATTR — pas à une coupure d'alimentation) : sert de
// remplacement à Serial.print*/println/printf dans tout le firmware.
// Exposé côté web via GET /serial (cf. http_manager.cpp).
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <Arduino.h>

// ---- OBJETS GLOBAUX ----

// Taille du journal. Exposée ici et non dans le .cpp :
// tout appelant qui compose une ligne longue (cf. la synthèse [LOOP] de main.cpp)
// doit dimensionner son tampon dessus
#define LOG_LINE_LEN  100
#define LOG_MAX_LINES 40

// ---- API PUBLIQUES ----

// À appeler tôt dans setup() (avant les premiers log_line()). Ne vide le
// buffer que sur un vrai démarrage à froid (ESP_RST_POWERON) — sinon
// (crash, watchdog, reset logiciel), les lignes d'avant le reset sont
// conservées et les nouvelles s'ajoutent à la suite.
void log_init();

// Remplace Serial.print*/println/printf : imprime sur Serial (comportement
// moniteur série inchangé) ET conserve la ligne dans le buffer circulaire.
void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// JSON des max_lines dernières lignes (les plus récentes en dernier),
// pour la route HTTP GET /serial.
String log_get_json(int max_lines = 20);

// Vide le journal (bouton "Effacer" du panneau Logs de la page web). Utile pour
// isoler ce qui suit une action précise, le buffer ne faisant que 40 lignes.
void log_clear();

// Journalisation des mesures de boucle ([LOOP] de main.cpp) — COUPÉE au boot :
// une ligne de synthèse toutes les 10 s plus les dépassements de 20 ms
// évacuent un journal de 40 lignes avant qu'on ait pu lire autre chose.
// La mesure, elle, tourne toujours : réactiver donne une fenêtre complète en
// 10 s. Bouton "Mesure boucle" du panneau Diagnostic, commande "loop:on|off".
void log_set_loop_measure(bool on);
bool log_loop_measure();
