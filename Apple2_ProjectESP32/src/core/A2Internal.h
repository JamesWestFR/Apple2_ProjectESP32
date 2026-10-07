/*

Apple2_ProjectESP32 — Déclarations partagées entre les fichiers du cœur.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef A2Internal_h
#define A2Internal_h

#include "A2.h"

namespace A2 {

uint8_t* ramMain();
uint8_t* ramAux();
void videoSetModel();
int videoLineCycle();      // position du balayage dans la ligne, en cycles (0 à 64)
void slotsChanged();       // une carte est apparue ou a disparu : refait les pages $C100-$CEFF

namespace Hdd {
const uint8_t* romPage();  // firmware de la carte en $C700, nul sans disque
}

}

#endif // A2Internal_h
