/*

Apple2_ProjectESP32 — Sortie VGA.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef Video_h
#define Video_h

#include <stdint.h>

namespace Video {

// Mis à vrai par l'interruption vidéo à la fin de la dernière ligne d'image
extern volatile bool vsync;
extern bool ready;

void init();
void setMonitor(uint8_t type);      // MONITOR_* : couleur ou moniteur monochrome
void setScanlines(bool on);
void clear();
// Texte par-dessus l'image, dans la grille de 40 x 24 caractères de l'Apple.
// `inverse` : noir sur blanc.
void overlayText(int col, int row, const char* text, bool inverse = true);
// Une ligne de 40 codes écran Apple, dessinée dans la ligne de texte `row`
void drawTextRow(int row, const uint8_t* chars, uint8_t fg, uint8_t bg);
bool screenshot(const char* path);
// Diagnostic : nombre de points non noirs du framebuffer, par ligne de texte
void printSummary();
// Diagnostic : tout le framebuffer en hexadécimal sur le port série
void dumpFramebuffer();

}

#endif // Video_h
