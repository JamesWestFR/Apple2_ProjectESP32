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
void romBankChanged();     // //c : l'autre moitié de la ROM vient d'être choisie
void slotsChanged();       // une carte est apparue ou a disparu : refait les pages $C100-$CEFF
void cpuState(StateIO& io);

// Ligne d'interruption du 6502 : elle est tenue tant qu'une carte la demande
enum : uint8_t { IRQ_MOCKINGBOARD = 1, IRQ_MOUSE = 2, IRQ_IIC_VBL = 4, IRQ_IIC_MOUSE = 8 };
void setIrq(uint8_t source, bool on);

namespace Hdd {
const uint8_t* romPage();  // firmware de la carte en $C700, nul sans disque
}

namespace Disk {
void state(StateIO& io);
// //c Plus : lignes du circuit MIG (lecteur interne sur le lecteur 2, lecteurs
// 3,5 pouces externes choisis, ligne SEL)
void setMig(bool internalDrive, bool external35, bool sel);
void setPlus(bool on);
}

// Lecteur 3,5 pouces interne du //c Plus
namespace Disk35 {
void init();
void setPresent(bool on);
bool isMedia(uint32_t blocks);    // l'image est une disquette de ce lecteur
void mediaChanged();
void reset();
bool busy();
void setSide(bool side);
bool sense(uint8_t reg);
void command(uint8_t reg);
uint8_t readData();
void writeData(uint8_t v);
void writeEnd();
uint8_t handshake();
void state(StateIO& io);
}

// Disque SmartPort sur la prise du lecteur externe du //c
namespace SmartPort {
void setEnabled(bool on);
void setInternal35(bool on);
void busReset();
bool ack();
void phases(uint8_t p);
void write(uint8_t v);
uint8_t read();
void state(StateIO& io);
}

// Souris, interruption de retour vertical et ports série du //c
namespace IIc {
void reset();
int read(uint8_t reg, uint8_t keyBits);     // -1 : adresse d'un //e ordinaire
bool write(uint8_t reg, uint8_t value);
void scanlineTick();
void vbl();
void mouseMove(int dx, int dy);
void mouseButton(bool down);
bool romBank();                             // moitié haute de la ROM de 32 Ko en service
void setRomVersion(bool banked, bool expansion, bool plus);
int migRead(uint16_t addr);                 // //c Plus, $CC00 et $CE00 : -1 si la ROM répond
bool migWrite(uint16_t addr, uint8_t value);
bool migVisible();                          // le circuit MIG occupe $CC00 et $CE00
void state(StateIO& io);
}
namespace Mouse {
const uint8_t* romPage();  // page de firmware visible en $C200, nul sans carte
void state(StateIO& io);
}
namespace Mockingboard { void state(StateIO& io); }

}

#endif // A2Internal_h
