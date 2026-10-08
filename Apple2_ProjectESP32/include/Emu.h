/*

Apple2_ProjectESP32, an Apple II emulator for Espressif ESP32 SoC

Initialisation et boucle principale. Architecture reprise d'ESPectrum
(Víctor Iborra, David Crespo) : https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef Emu_h
#define Emu_h

#include <string>
#include "fabgl.h"

// Son : 520 échantillons par image, soit 31 169 Hz à 59,94 images par seconde
#define AUDIO_SAMPLES_PER_FRAME 520
#define AUDIO_RATE 31169
// Durée d'une image VGA, en microsecondes
#define MICROS_PER_FRAME 16683

namespace Emu {

extern fabgl::PS2Controller PS2Controller;

extern volatile bool paused;
extern bool turbo;                 // vitesse maximale (Arrêt défil)
extern bool resetRequest;          // Ctrl-Reset
extern bool coldBootRequest;       // mise sous tension
extern bool screenshotRequest;
extern uint8_t stateRequest;        // 1 : sauvegarder l'état, 2 : le reprendre
extern bool forceRedraw;           // diagnostic : toute l'image est redessinée à chaque fois

// Vitesse mesurée sur la dernière seconde : images affichées et images émulées
// par seconde, durée de calcul d'une image avec son rendu
extern float statFps, statEmulatedFps, statFrameMs;

void setup();
void loop();
void showNotice(const char* text);
// Nom de l'image dont dépendent les réglages par jeu : le disque dur, sinon la disquette du lecteur 1
std::string gameName();

}

#endif // Emu_h
