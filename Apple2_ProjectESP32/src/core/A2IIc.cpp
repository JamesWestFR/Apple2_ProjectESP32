/*

Apple2_ProjectESP32 — Ce que l'Apple //c a en propre : la souris, l'interruption
de retour vertical et les deux ports série.

Souris : pas de carte, la souris est reliée à l'IOU. Chaque axe donne deux
signaux en quadrature ; un front du premier (X0, Y0) peut lever une
interruption, et le second (X1, Y1) dit alors le sens. Le firmware du //c
compte ces fronts. Les bascules de l'IOU ($C058-$C05F) choisissent le front et
autorisent les interruptions ; elles ne sont accessibles que si IOUDIS est
baissé ($C079), sinon ces adresses restent celles du //e (annonciateurs,
double haute résolution).

Retour vertical : $C05B autorise une interruption à chaque image, que la
lecture de $C070 acquitte ; $C019 dit si elle est en attente.

Ports série : deux ACIA 6551, en $C098 (port 1, l'imprimante) et $C0A8 (port 2,
le modem). L'émetteur est toujours libre et rien n'est jamais reçu ; ce que le
port 1 émet est remis à la plateforme, qui l'écrit dans un fichier.

Comportement repris de MAME (apple2e.cpp).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {
namespace IIc {

struct State {
    // IOU
    bool iouDisabled;               // $C058-$C05F sont les adresses du //e, pas les bascules de l'IOU
    bool xyMask, vblMask;           // interruptions de la souris et du retour vertical autorisées
    bool x0Edge, y0Edge;            // front qui interrompt : faux montant, vrai descendant
    bool x0, x1, y0, y1;            // signaux en quadrature de la souris
    bool xIrq, yIrq, vblIrq;        // interruptions en attente
    bool button;
    int16_t countX, countY;         // pas de souris qui restent à transmettre
    // ACIA 6551
    uint8_t command[2], control[2];
    // ROM de 32 Ko : moitié en service
    bool romHigh;
    // Extension mémoire ($C0C0-$C0C3) : adresse sur 24 bits, qui avance à chaque accès à la donnée
    uint32_t expAddress;
};

static State s;
static bool bankedRom = false, hasExpansion = false;
static uint8_t* expRam = nullptr;
static uint32_t expMask = 0;

bool romBank() { return s.romHigh; }

void setRomVersion(bool banked, bool expansion) {
    bankedRom = banked;
    hasExpansion = expansion;
}

void setExpansion(uint8_t* ram, uint32_t size) {
    expRam = ram;
    expMask = size ? size - 1 : 0;
}

void reset() {
    int16_t cx = s.countX, cy = s.countY;
    bool button = s.button;
    memset(&s, 0, sizeof(s));
    s.iouDisabled = true;
    s.button = button;
    s.countX = cx;
    s.countY = cy;
    setIrq(IRQ_IIC_VBL | IRQ_IIC_MOUSE, false);
}

void mouseMove(int dx, int dy) {
    // Le firmware ne compte qu'un front de X0 sur deux : deux pas par unité de déplacement
    int x = s.countX + 2 * dx, y = s.countY + 2 * dy;
    // Une souris ne donne pas plus de quelques centaines de pas par image
    s.countX = (int16_t)(x > 1000 ? 1000 : (x < -1000 ? -1000 : x));
    s.countY = (int16_t)(y > 1000 ? 1000 : (y < -1000 ? -1000 : y));
}

void mouseButton(bool down) { s.button = down; }

// Un pas de souris au plus par axe toutes les quatre lignes de balayage : le
// firmware traite une interruption par front, et en perdrait à un rythme plus
// soutenu (une vraie souris ne va pas plus vite)
void scanlineTick() {
    if (scanline & 3) return;
    bool irq = false;
    if (s.countX) {
        if (s.countX < 0) { s.countX++; s.x1 = false; }
        else { s.countX--; s.x1 = true; }
        // X0 change d'état : interruption si c'est le front choisi
        if (s.x0 == s.x0Edge && s.xyMask) { s.xIrq = true; irq = true; }
        s.x0 = !s.x0;
    }
    if (s.countY) {
        if (s.countY < 0) { s.countY++; s.y1 = true; }
        else { s.countY--; s.y1 = false; }
        if (s.y0 == s.y0Edge && s.xyMask) { s.yIrq = true; irq = true; }
        s.y0 = !s.y0;
    }
    if (irq) setIrq(IRQ_IIC_MOUSE, true);
}

void vbl() {
    if (!s.vblMask) return;
    s.vblIrq = true;
    setIrq(IRQ_IIC_VBL, true);
}

// Bascules de l'IOU, quand IOUDIS est baissé
static void iouSwitch(uint8_t reg) {
    switch (reg) {
        case 0x58: s.xyMask = false; break;
        case 0x59: s.xyMask = true; break;
        case 0x5A: s.vblMask = false; s.vblIrq = false; setIrq(IRQ_IIC_VBL, false); break;
        case 0x5B: s.vblMask = true; break;
        case 0x5C: s.x0Edge = false; break;
        case 0x5D: s.x0Edge = true; break;
        case 0x5E: s.y0Edge = false; break;
        default:   s.y0Edge = true; break;
    }
}

// Effets communs à la lecture et à l'écriture. Vrai si l'adresse est traitée ici.
static bool access(uint8_t reg, bool isWrite) {
    if (reg == 0x48) {
        s.xIrq = s.yIrq = false;
        setIrq(IRQ_IIC_MOUSE, false);
        return true;
    }
    if (reg >= 0x58 && reg <= 0x5F && !s.iouDisabled) {
        iouSwitch(reg);
        return true;
    }
    if ((reg & 0xF0) == 0x70) {
        // Toute la zone $C07x acquitte l'interruption de retour vertical
        s.vblIrq = false;
        setIrq(IRQ_IIC_VBL, false);
        if (isWrite && reg >= 0x78) s.iouDisabled = !(reg & 1);
    }
    return false;
}

static inline int aciaIndex(uint8_t reg) {
    return (reg & 0xFC) == 0x98 ? 0 : ((reg & 0xFC) == 0xA8 ? 1 : -1);
}

// Extension mémoire : les trois octets de l'adresse (le quartet haut du
// troisième est toujours à 1 sur une carte de 1 Mo au plus), puis la donnée
static int expansionAccess(uint8_t reg, bool isWrite, uint8_t value) {
    if (!hasExpansion || !expRam || (reg & 0xF0) != 0xC0) return -1;
    int r = reg & 3;
    if (r == 3) {
        uint32_t a = s.expAddress & 0xFFFFF;
        int v = 0xFF;
        if (a <= expMask) {
            if (isWrite) expRam[a] = value;
            v = expRam[a];
        }
        s.expAddress = (s.expAddress + 1) & 0xFFFFFF;
        return v;
    }
    if (isWrite) {
        int shift = r * 8;
        s.expAddress = (s.expAddress & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return 0;
    }
    return r == 2 ? (((s.expAddress >> 16) & 0xFF) | 0xF0) : ((s.expAddress >> (r * 8)) & 0xFF);
}

// Rend la valeur lue, ou -1 si l'adresse est celle d'un //e ordinaire
int read(uint8_t reg, uint8_t keyBits) {
    if (reg == 0x28 && bankedRom) {
        s.romHigh = !s.romHigh;
        romBankChanged();
        return 0;
    }
    int e = expansionAccess(reg, false, 0);
    if (e >= 0) return e;
    switch (reg) {
        case 0x15: setIrq(IRQ_IIC_MOUSE, false); return (s.xIrq ? 0x80 : 0) | keyBits;
        case 0x17: setIrq(IRQ_IIC_MOUSE, false); return (s.yIrq ? 0x80 : 0) | keyBits;
        case 0x19: return (s.vblIrq ? 0x80 : 0) | keyBits;
        case 0x40: return s.xyMask ? 0x80 : 0;
        case 0x41: return s.vblMask ? 0x80 : 0;
        case 0x42: return s.x0Edge ? 0x80 : 0;
        case 0x43: return s.y0Edge ? 0x80 : 0;
        case 0x60: case 0x68: return 0x80;                          // interrupteur 40/80 colonnes, sur 40
        case 0x63: case 0x6B: return s.button ? 0 : 0x80;           // bouton de la souris, inversé
        case 0x66: case 0x6E: return s.x1 ? 0x80 : 0;
        case 0x67: case 0x6F: return s.y1 ? 0x80 : 0;
        default: break;
    }
    int port = aciaIndex(reg);
    if (port >= 0) {
        switch (reg & 3) {
            case 0: return 0;                       // rien de reçu
            case 1: return 0x10;                    // émetteur libre, porteuse et DSR présents
            case 2: return s.command[port];
            default: return s.control[port];
        }
    }
    if (access(reg, false)) return 0;
    // $C078, $C07A, $C07C, $C07E : état de IOUDIS (après l'acquittement ci-dessus)
    if (reg >= 0x78 && reg <= 0x7E && !(reg & 1)) return s.iouDisabled ? 0x80 : 0;
    return -1;
}

// Vrai si l'écriture est traitée ici
bool write(uint8_t reg, uint8_t value) {
    if (reg == 0x28 && bankedRom) {
        s.romHigh = !s.romHigh;
        romBankChanged();
        return true;
    }
    if (expansionAccess(reg, true, value) >= 0) return true;
    int port = aciaIndex(reg);
    if (port >= 0) {
        switch (reg & 3) {
            case 0: A2_platformSerialOut(port, value); break;
            case 1: break;                          // remise à zéro programmée
            case 2: s.command[port] = value; break;
            default: s.control[port] = value; break;
        }
        return true;
    }
    return access(reg, true);
}

void state(StateIO& io) {
    io.bytes(&s, sizeof(s));
    if (!io.saving) {
        setIrq(IRQ_IIC_VBL, s.vblIrq);
        setIrq(IRQ_IIC_MOUSE, s.xIrq || s.yIrq);
    }
}

}

void setExpansionRam(uint8_t* ram, uint32_t size) { IIc::setExpansion(ram, size); }

}
