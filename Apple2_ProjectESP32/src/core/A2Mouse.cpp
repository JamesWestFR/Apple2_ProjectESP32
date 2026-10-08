/*

Apple2_ProjectESP32 — Carte souris Apple (AppleMouse II), slot 2.

La carte d'origine associe un PIA 6821 à un microcontrôleur 68705 ; son
firmware (ROM 341-0270-C, huit pages de 256 octets choisies par le port B du
PIA) dialogue avec le microcontrôleur octet par octet. C'est ce dialogue qui
est émulé ici, pas le microcontrôleur : la ROM d'origine tourne telle quelle.

Portage de MouseInterface.cpp et 6821.cpp d'AppleWin (GPL), eux-mêmes issus de
« Apple in PC » de Kyle Kim.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"
#include "roms_apple2.h"

namespace A2 {
namespace Mouse {

bool enabled = false;

// Commandes du firmware (quartet haut du premier octet)
enum : uint8_t {
    CMD_SET = 0x00, CMD_READ = 0x10, CMD_SERVE = 0x20, CMD_CLEAR = 0x30, CMD_POS = 0x40,
    CMD_INIT = 0x50, CMD_CLAMP = 0x60, CMD_HOME = 0x70, CMD_TIME = 0x90,
};

// Octet d'état
enum : uint8_t {
    STAT_PREV_BUTTON1 = 0x01, STAT_INT_MOVEMENT = 0x02, STAT_INT_BUTTON = 0x04, STAT_INT_VBL = 0x08,
    STAT_CURR_BUTTON1 = 0x10, STAT_MOVED = 0x20, STAT_PREV_BUTTON0 = 0x40, STAT_CURR_BUTTON0 = 0x80,
    STAT_INT_ALL = STAT_INT_VBL | STAT_INT_BUTTON | STAT_INT_MOVEMENT,
};

// Octet de mode : souris en service, puis les mêmes bits d'interruption que l'état
enum : uint8_t { MODE_MOUSE_ON = 0x01 };

struct State {
    // PIA 6821
    uint8_t pra, ddra, cra, prb, ddrb, crb;
    uint8_t inA, inB;               // ce que le microcontrôleur présente sur les ports
    // Dialogue avec le microcontrôleur
    uint8_t portA, portB;
    uint8_t buffer[8];
    uint8_t bufferPos, dataLen;
    uint8_t status, mode;
    // Souris : position rendue au dernier READ, position courante, bornes
    int32_t readX, readY, x, y, minX, maxX, minY, maxY;
    bool readButton[2], button[2];
};

static State s;

const uint8_t* romPage() {
    if (!enabled) return nullptr;
    // Les bits 1 à 3 du port B choisissent la page de ROM visible en $C200
    return gb_rom_mouse + ((s.portB << 7) & 0x0700);
}

static void clampPosition() {
    if (s.x > s.maxX) s.x = s.maxX;
    if (s.x < s.minX) s.x = s.minX;
    if (s.y > s.maxY) s.y = s.maxY;
    if (s.y < s.minY) s.y = s.minY;
}

static void setClamp(int32_t& lo, int32_t& hi, int32_t minV, int32_t maxV) {
    if (minV > maxV) {
        maxV = (minV + maxV) & 0xFFFF;
        minV = 0;
    }
    lo = minV;
    hi = maxV;
    clampPosition();
}

static void clearMouse() {
    s.bufferPos = 0;
    s.dataLen = 1;
    s.status = 0;
    s.readX = s.readY = 0;
    s.readButton[0] = s.readButton[1] = false;
    s.x = s.y = 0;
}

// Mouvement, bouton ou retour vertical : met à jour l'état et lève l'interruption demandée
static void mouseEvent(bool vbl) {
    uint8_t st = 0;
    if ((s.mode & STAT_INT_VBL) && vbl) st |= STAT_INT_VBL;
    if (s.mode & MODE_MOUSE_ON) {
        if (s.readX != s.x || s.readY != s.y) {
            st |= STAT_INT_MOVEMENT | STAT_MOVED;
            s.status |= STAT_MOVED;
        }
        if (s.readButton[0] != s.button[0] || s.readButton[1] != s.button[1]) st |= STAT_INT_BUTTON;
        st &= (s.mode & STAT_INT_ALL) | STAT_MOVED;
    } else {
        // Souris hors service : seule l'interruption de retour vertical reste possible
        st &= STAT_INT_VBL;
    }
    if (st & STAT_INT_ALL) {
        s.status |= st;
        setIrq(IRQ_MOUSE, true);
    }
}

// Premier octet d'une commande : sa longueur, et la réponse à préparer
static void onCommand() {
    switch (s.buffer[0] & 0xF0) {
        case CMD_SET:
            s.dataLen = 1;
            s.mode = s.buffer[0] & 0x0F;
            break;
        case CMD_READ:
            s.dataLen = 6;
            s.status &= STAT_MOVED;
            s.readX = s.x;
            s.readY = s.y;
            if (s.readButton[0]) s.status |= STAT_PREV_BUTTON0;
            if (s.readButton[1]) s.status |= STAT_PREV_BUTTON1;
            s.readButton[0] = s.button[0];
            s.readButton[1] = s.button[1];
            if (s.readButton[0]) s.status |= STAT_CURR_BUTTON0;
            if (s.readButton[1]) s.status |= STAT_CURR_BUTTON1;
            s.buffer[1] = (uint8_t)s.readX;
            s.buffer[2] = (uint8_t)(s.readX >> 8);
            s.buffer[3] = (uint8_t)s.readY;
            s.buffer[4] = (uint8_t)(s.readY >> 8);
            s.buffer[5] = s.status;
            s.status &= ~STAT_MOVED;
            break;
        case CMD_SERVE:
            // Cause de l'interruption ; la ligne retombe
            s.dataLen = 2;
            s.buffer[1] = s.status & ~STAT_MOVED;
            setIrq(IRQ_MOUSE, false);
            break;
        case CMD_CLEAR:
            clearMouse();           // les bornes restent en place
            s.dataLen = 1;
            break;
        case CMD_POS:
        case CMD_CLAMP:
            s.dataLen = 5;
            break;
        case CMD_INIT:
            s.dataLen = 3;
            s.buffer[1] = 0xFF;
            break;
        case CMD_HOME:
            s.dataLen = 1;
            s.x = s.y = 0;
            break;
        case CMD_TIME:
            // Suivi d'octets de réglage selon les bits 2 et 3
            switch (s.buffer[0] & 0x0C) {
                case 0x00: s.dataLen = 1; break;
                case 0x04: s.dataLen = 3; break;
                case 0x08: s.dataLen = 2; break;
                default:   s.dataLen = 4; break;
            }
            break;
        case 0xA0:
            s.dataLen = 2;
            break;
        default:
            s.dataLen = 1;
            break;
    }
    s.inA = s.buffer[1];
}

// Commande reçue en entier
static void onWrite() {
    switch (s.buffer[0] & 0xF0) {
        case CMD_CLAMP: {
            int32_t lo = (s.buffer[3] << 8) | s.buffer[1];
            int32_t hi = (s.buffer[4] << 8) | s.buffer[2];
            if (s.buffer[0] & 1) setClamp(s.minY, s.maxY, lo, hi);
            else setClamp(s.minX, s.maxX, lo, hi);
            break;
        }
        case CMD_POS:
            s.readX = s.x = (s.buffer[2] << 8) | s.buffer[1];
            s.readY = s.y = (s.buffer[4] << 8) | s.buffer[3];
            break;
        case CMD_INIT:
            s.readX = s.readY = 0;
            s.minX = s.minY = 0;
            s.maxX = s.maxY = 1023;
            s.x = s.y = 0;
            break;
        default:
            break;
    }
}

// Port B du PIA : les bits 4 et 5 cadencent l'échange avec le microcontrôleur
static void onPortB(uint8_t data) {
    uint8_t diff = (s.portB ^ data) & 0x3E;
    if (!diff) return;
    uint8_t oldPage = s.portB & 0x0E;
    s.portB = (s.portB & ~0x3E) | (data & 0x3E);
    if (diff & 0x20) {
        if (data & 0x20) {
            s.portB |= 0x80;        // prêt à recevoir un octet
        } else {
            // Un octet arrive du 6502
            if (s.bufferPos < 8) s.buffer[s.bufferPos++] = s.portA;
            if (s.bufferPos == 1) onCommand();
            if (s.bufferPos == s.dataLen || s.bufferPos > 7) {
                onWrite();
                s.bufferPos = 0;
            }
            s.portB &= ~0x80;
            s.inB = s.portB;
        }
    }
    if (diff & 0x10) {
        if (data & 0x10) {
            s.portB &= ~0x40;       // l'octet suivant se prépare
        } else {
            // Le 6502 lit un octet de la réponse
            if (s.bufferPos) s.bufferPos++;
            if (s.bufferPos == s.dataLen || s.bufferPos > 7) s.bufferPos = 0;
            else s.inA = s.buffer[s.bufferPos];
            s.portB |= 0x40;
        }
    }
    s.inB = s.portB;
    if ((s.portB & 0x0E) != oldPage) slotsChanged();
}

uint8_t read(uint8_t reg) {
    switch (reg & 3) {
        case 0:
            if (!(s.cra & 0x04)) return s.ddra;
            s.cra &= 0x3F;
            return (s.pra & s.ddra) | (s.inA & ~s.ddra);
        case 1: return s.cra;
        case 2:
            if (!(s.crb & 0x04)) return s.ddrb;
            s.crb &= 0x3F;
            return (s.prb & s.ddrb) | (s.inB & ~s.ddrb);
        default: return s.crb;
    }
}

void write(uint8_t reg, uint8_t value) {
    switch (reg & 3) {
        case 0:
            if (s.cra & 0x04) {
                s.pra = value;
            } else {
                if (s.ddra == value) return;
                s.ddra = value;
            }
            if (s.ddra) s.portA = s.pra & s.ddra;
            break;
        case 1: s.cra = value; break;
        case 2:
            if (s.crb & 0x04) {
                s.prb = value;
            } else {
                if (s.ddrb == value) return;
                s.ddrb = value;
            }
            if (s.ddrb) onPortB(s.prb & s.ddrb);
            break;
        default: s.crb = value; break;
    }
}

void reset() {
    memset(&s, 0, sizeof(s));
    s.portB = 0x40;
    s.inB = s.portB;
    s.maxX = s.maxY = 1023;
    s.dataLen = 1;
    setIrq(IRQ_MOUSE, false);
    if (enabled) slotsChanged();
}

void setEnabled(bool on) {
    enabled = on;
    reset();
    slotsChanged();
}

void move(int dx, int dy) {
    if (isIIc()) { IIc::mouseMove(dx, dy); return; }
    if (!enabled || (dx == 0 && dy == 0)) return;
    s.x += dx;
    s.y += dy;
    clampPosition();
    mouseEvent(false);
}

void setButton(int n, bool down) {
    if (isIIc()) { if (n == 0) IIc::mouseButton(down); return; }
    if (!enabled || n < 0 || n > 1 || s.button[n] == down) return;
    s.button[n] = down;
    mouseEvent(false);
}

void vbl() {
    if (enabled) mouseEvent(true);
}

void state(StateIO& io) {
    io.bytes(&s, sizeof(s));
    if (!io.saving) setIrq(IRQ_MOUSE, enabled && (s.status & s.mode & STAT_INT_ALL) != 0);
}

}
}
