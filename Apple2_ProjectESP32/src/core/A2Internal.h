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
void cpuState(StateIO& io);

// Ligne d'interruption du 6502 : elle est tenue tant qu'une carte la demande
enum : uint8_t { IRQ_MOCKINGBOARD = 1, IRQ_MOUSE = 2, IRQ_IIC_VBL = 4, IRQ_IIC_MOUSE = 8 };
void setIrq(uint8_t source, bool on);

namespace Hdd {
const uint8_t* romPage();  // firmware de la carte en $C700, nul sans disque
}

namespace Disk { void state(StateIO& io); }

// Souris, interruption de retour vertical et ports série du //c
namespace IIc {
void reset();
int read(uint8_t reg, uint8_t keyBits);     // -1 : adresse d'un //e ordinaire
bool write(uint8_t reg, uint8_t value);
void scanlineTick();
void vbl();
void mouseMove(int dx, int dy);
void mouseButton(bool down);
void state(StateIO& io);
}
namespace Mouse {
const uint8_t* romPage();  // page de firmware visible en $C200, nul sans carte
void state(StateIO& io);
}
namespace Mockingboard { void state(StateIO& io); }

}

#endif // A2Internal_h
