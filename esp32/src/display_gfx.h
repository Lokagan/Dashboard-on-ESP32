#pragma once

// ============================================================
// DISPLAY_GFX.H — rastériseur logiciel sur un buffer RGB565.
// Aucune notion de bus ni de dalle : il dessine en mémoire, un autre module
// pousse le résultat (cf. display_driver.h).
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <stdint.h>
#include <stddef.h>

namespace gfx {

// ---- OBJETS GLOBAUX ----

// Cible de dessin. Les coordonnées sont clippées, jamais vérifiées par
// l'appelant : une page qui déborde ne corrompt pas la mémoire voisine.
struct Canvas {
    uint16_t* px;   // RGB565, ordre natif de la dalle (cf. display_driver.h)
    int16_t   w, h;
};

// Police 5x7 dans une cellule de 6x8 — toute la géométrie des écrans en dépend.
constexpr int CHAR_W = 6;
constexpr int CHAR_H = 8;

// ---- API PUBLIQUES ----

void fill(const Canvas& c, uint16_t color);
void fill_rect(const Canvas& c, int x, int y, int w, int h, uint16_t color);
void rect(const Canvas& c, int x, int y, int w, int h, uint16_t color);
void hline(const Canvas& c, int x, int y, int w, uint16_t color);
void vline(const Canvas& c, int x, int y, int h, uint16_t color);
void pixel(const Canvas& c, int x, int y, uint16_t color);

// Anneau de progression. Départ en HAUT, sens horaire. frac 0..1 peint en fg,
// le reste en bg. Bords francs. Se dessine bande par bande : le clipping
// suffit, il n'y a qu'à décaler cy.
void ring(const Canvas& c, int cx, int cy, int r_out, int r_in,
          float frac, uint16_t fg, uint16_t bg);

// Texte ASCII. bg == fg laisse le fond intact (dessin transparent).
void text(const Canvas& c, int x, int y, const char* s,
          uint16_t fg, uint16_t bg, uint8_t size = 1);

// Largeur d'une chaîne, pour aligner à droite ou centrer sans compter à la main.
// ⚠️ Largeur d'AVANCE : la colonne d'espacement du dernier caractère est COMPRISE.
// Ce n'est pas un bug de 1 px — la plupart des appelants enchaînent dessus
// (`x + text_w(cle)` pour poser une valeur après son intitulé) et comptent sur ce
// pixel de queue. Le « corriger » décalerait tout le monde pour aligner deux appels.
int text_w(const char* s, uint8_t size = 1);

}  // namespace gfx
