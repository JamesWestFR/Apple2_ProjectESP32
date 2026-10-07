/*

Apple2_ProjectESP32 — Carte son Mockingboard (slot 4).

Deux 6522 (VIA), en $C400 et $C480, qui pilotent chacun un générateur de son
AY-3-8910 : le port A porte les données, le port B les lignes de commande. Le
compteur 1 de chaque 6522 fournit l'interruption qui cadence la musique.

Les compteurs ne sont pas décrémentés à chaque cycle : leur valeur se déduit de
l'instant de leur chargement, et l'instant de la prochaine interruption est
calculé à l'avance (le 6502 ne fait que le comparer au temps courant).

Les écritures dans les registres des AY sont notées avec leur instant dans
l'image et rejouées à leur place quand le son de l'image est produit.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {

bool irqLine = false;
bool timerArmed = false;
uint32_t timerDue = 0;

namespace Mockingboard {

bool enabled = true;

// --- 6522 --------------------------------------------------------------------

enum : uint8_t { IFR_T2 = 0x20, IFR_T1 = 0x40 };

// Registres des AY tels que le programme les a écrits (les puces elles-mêmes
// ne sont mises à jour qu'au rendu du son) : c'est ce que rend une relecture
static uint8_t regShadow[2][16];

struct Via {
    uint8_t orb, ora, ddrb, ddra, acr, pcr, ifr, ier;
    uint16_t t1Latch, t2Latch;
    uint32_t t1Start, t2Start;      // instant du chargement du compteur
    bool t1Armed, t2Armed;          // une interruption est encore à venir
    uint8_t ayAddr;                 // registre de l'AY sélectionné
};

static Via via[2];

// --- AY-3-8910 ---------------------------------------------------------------

struct Ay {
    uint8_t reg[16];
    // État du générateur
    uint16_t toneCount[3];
    uint8_t toneOut[3];
    uint16_t noiseCount;
    uint32_t noiseShift;
    uint32_t envCount;
    uint8_t envStep;                // 0 à 31 dans la forme en cours
    bool envHolding;
    bool envAttack;
};

static Ay ay[2];

// Écritures de l'image en cours, à rejouer par le rendu du son
struct AyEvent {
    uint16_t time;                  // cycles depuis le début de l'image
    uint8_t chipReg;                // puce << 4 | registre
    uint8_t value;
};
#define AY_MAX_EVENTS 512
static AyEvent events[AY_MAX_EVENTS];
static int eventCount = 0;
static uint32_t frameStart = 0;
static uint32_t tickFraction = 0;

// Niveaux de sortie de l'AY : 3 dB par pas
static const uint8_t volumeTable[16] = { 0, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 90, 128, 180, 255 };

static void ayReset(Ay& a) {
    memset(&a, 0, sizeof(a));
    a.reg[7] = 0xFF;
    a.noiseShift = 1;
}

static void ayWrite(Ay& a, uint8_t reg, uint8_t value) {
    reg &= 15;
    a.reg[reg] = value;
    if (reg == 13) {
        // Écrire la forme relance l'enveloppe
        a.envCount = 0;
        a.envStep = 0;
        a.envHolding = false;
        a.envAttack = (value & 0x04) != 0;
    }
}

static inline uint8_t envLevel(const Ay& a) {
    uint8_t step = a.envStep & 15;
    return a.envAttack ? step : (15 - step);
}

// Un pas de l'horloge interne (horloge de l'Apple / 8) ; rend la somme des trois voies
static inline uint32_t ayTick(Ay& a) {
    // Tons : fréquence = horloge / (16 x période), la sortie bascule donc tous
    // les `période` pas
    for (int ch = 0; ch < 3; ch++) {
        uint16_t period = a.reg[ch * 2] | ((a.reg[ch * 2 + 1] & 0x0F) << 8);
        if (period < 1) period = 1;
        if (++a.toneCount[ch] >= period) {
            a.toneCount[ch] = 0;
            a.toneOut[ch] ^= 1;
        }
    }
    // Bruit : registre à décalage de 17 bits
    uint16_t noisePeriod = (a.reg[6] & 0x1F) * 2;
    if (noisePeriod < 2) noisePeriod = 2;
    if (++a.noiseCount >= noisePeriod) {
        a.noiseCount = 0;
        a.noiseShift = (a.noiseShift >> 1) | (((a.noiseShift ^ (a.noiseShift >> 3)) & 1) << 16);
    }
    // Enveloppe : 16 pas par cycle de la forme
    if (!a.envHolding) {
        uint32_t envPeriod = (a.reg[11] | (a.reg[12] << 8)) * 2;
        if (envPeriod < 2) envPeriod = 2;
        if (++a.envCount >= envPeriod) {
            a.envCount = 0;
            if (++a.envStep >= 16) {
                uint8_t shape = a.reg[13];
                if (!(shape & 0x08)) {
                    // Formes 0 à 7 : un seul cycle, puis silence
                    a.envHolding = true;
                    a.envAttack = false;
                    a.envStep = 15;
                } else if (shape & 0x01) {
                    // Maintien, sur le dernier niveau ou son opposé
                    a.envHolding = true;
                    if (shape & 0x02) a.envAttack = !a.envAttack;
                    a.envStep = 15;
                } else {
                    if (shape & 0x02) a.envAttack = !a.envAttack;
                    a.envStep = 0;
                }
            }
        }
    }

    uint8_t mixer = a.reg[7];
    uint8_t noise = a.noiseShift & 1;
    uint32_t sum = 0;
    for (int ch = 0; ch < 3; ch++) {
        bool toneOn = a.toneOut[ch] || (mixer & (1 << ch));
        bool noiseOn = noise || (mixer & (8 << ch));
        if (!(toneOn && noiseOn)) continue;
        uint8_t vol = a.reg[8 + ch];
        sum += volumeTable[(vol & 0x10) ? envLevel(a) : (vol & 0x0F)];
    }
    return sum;
}

static inline bool aySilent(const Ay& a) {
    return !((a.reg[8] | a.reg[9] | a.reg[10]) & 0x1F);
}

// --- Compteurs et interruptions ---------------------------------------------

static inline uint32_t t1Period(const Via& v) { return (uint32_t)v.t1Latch + 2; }

static void updateIrq() {
    bool line = false;
    bool armed = false;
    uint32_t due = 0;
    for (int i = 0; i < 2; i++) {
        Via& v = via[i];
        if (v.ifr & v.ier & 0x7F) line = true;
        uint32_t t;
        if (v.t1Armed) {
            t = v.t1Start + t1Period(v);
            if (!armed || (int32_t)(t - due) < 0) due = t;
            armed = true;
        }
        if (v.t2Armed) {
            t = v.t2Start + (uint32_t)v.t2Latch + 2;
            if (!armed || (int32_t)(t - due) < 0) due = t;
            armed = true;
        }
    }
    irqLine = line;
    timerArmed = armed;
    timerDue = due;
}

}

// Appelée par le 6502 quand l'instant timerDue est atteint
void timerEvent() {
    using namespace Mockingboard;
    for (int i = 0; i < 2; i++) {
        Via& v = via[i];
        if (v.t1Armed && (int32_t)(cycles - (v.t1Start + t1Period(v))) >= 0) {
            v.ifr |= IFR_T1;
            // Mode continu : le compteur est rechargé ; sinon une seule interruption
            if (v.acr & 0x40) v.t1Start += t1Period(v);
            else v.t1Armed = false;
        }
        if (v.t2Armed && (int32_t)(cycles - (v.t2Start + (uint32_t)v.t2Latch + 2)) >= 0) {
            v.ifr |= IFR_T2;
            v.t2Armed = false;
        }
    }
    updateIrq();
}

namespace Mockingboard {

static uint16_t t1Counter(const Via& v) {
    uint32_t elapsed = cycles - v.t1Start;
    if (v.acr & 0x40) elapsed %= t1Period(v);
    return (uint16_t)(v.t1Latch - elapsed);
}

static void pushEvent(uint8_t chipReg, uint8_t value) {
    if (eventCount >= AY_MAX_EVENTS) return;
    uint32_t t = cycles - frameStart;
    events[eventCount++] = { (uint16_t)(t > 0xFFFF ? 0xFFFF : t), chipReg, value };
}

// Port B : BC1 (bit 0), BDIR (bit 1), RESET (bit 2, actif à l'état bas).
// Bit 7 de chipReg : remise à zéro de la puce.
static void ayControl(int chip, Via& v) {
    switch (v.orb & 0x07) {
        case 0x00: case 0x01: case 0x02: case 0x03:
            memset(regShadow[chip], 0, 16);
            pushEvent((uint8_t)(chip << 4 | 0x80), 0);
            break;
        case 0x07:          // sélection du registre
            v.ayAddr = v.ora & 0x0F;
            break;
        case 0x06:          // écriture
            regShadow[chip][v.ayAddr] = v.ora;
            pushEvent((uint8_t)(chip << 4 | v.ayAddr), v.ora);
            break;
        case 0x05:          // lecture : le registre se présente sur le port A
            v.ora = regShadow[chip][v.ayAddr];
            break;
        default:
            break;
    }
}

uint8_t read(uint16_t addr) {
    int chip = (addr >> 7) & 1;
    Via& v = via[chip];
    switch (addr & 0x0F) {
        case 0x0: return v.orb;
        case 0x1: case 0xF: return v.ora;
        case 0x2: return v.ddrb;
        case 0x3: return v.ddra;
        case 0x4: v.ifr &= ~IFR_T1; updateIrq(); return (uint8_t)t1Counter(v);
        case 0x5: return (uint8_t)(t1Counter(v) >> 8);
        case 0x6: return (uint8_t)v.t1Latch;
        case 0x7: return (uint8_t)(v.t1Latch >> 8);
        case 0x8: v.ifr &= ~IFR_T2; updateIrq(); return (uint8_t)(v.t2Latch - (cycles - v.t2Start));
        case 0x9: return (uint8_t)((uint16_t)(v.t2Latch - (cycles - v.t2Start)) >> 8);
        case 0xB: return v.acr;
        case 0xC: return v.pcr;
        case 0xD: return v.ifr | ((v.ifr & v.ier & 0x7F) ? 0x80 : 0);
        case 0xE: return v.ier | 0x80;
        default: return 0;
    }
}

void write(uint16_t addr, uint8_t value) {
    int chip = (addr >> 7) & 1;
    Via& v = via[chip];
    switch (addr & 0x0F) {
        case 0x0: v.orb = value; ayControl(chip, v); break;
        case 0x1: case 0xF: v.ora = value; break;
        case 0x2: v.ddrb = value; break;
        case 0x3: v.ddra = value; break;
        case 0x4: case 0x6: v.t1Latch = (v.t1Latch & 0xFF00) | value; break;
        case 0x5:
            v.t1Latch = (v.t1Latch & 0x00FF) | (value << 8);
            v.t1Start = cycles;
            v.t1Armed = true;
            v.ifr &= ~IFR_T1;
            updateIrq();
            break;
        case 0x7:
            v.t1Latch = (v.t1Latch & 0x00FF) | (value << 8);
            v.ifr &= ~IFR_T1;
            updateIrq();
            break;
        case 0x8: v.t2Latch = (v.t2Latch & 0xFF00) | value; break;
        case 0x9:
            v.t2Latch = (v.t2Latch & 0x00FF) | (value << 8);
            v.t2Start = cycles;
            v.t2Armed = true;
            v.ifr &= ~IFR_T2;
            updateIrq();
            break;
        case 0xB: v.acr = value; updateIrq(); break;
        case 0xC: v.pcr = value; break;
        case 0xD: v.ifr &= ~value; updateIrq(); break;
        case 0xE:
            if (value & 0x80) v.ier |= value & 0x7F;
            else v.ier &= ~value;
            updateIrq();
            break;
        default: break;
    }
}

void setEnabled(bool on) {
    enabled = on;
    slotsChanged();
}

void reset() {
    for (int i = 0; i < 2; i++) {
        memset(&via[i], 0, sizeof(Via));
        via[i].t1Latch = via[i].t2Latch = 0xFFFF;
        via[i].t1Start = via[i].t2Start = cycles;
        ayReset(ay[i]);
    }
    memset(regShadow, 0, sizeof(regShadow));
    eventCount = 0;
    updateIrq();
}

static void applyEvent(const AyEvent& e) {
    Ay& a = ay[(e.chipReg >> 4) & 1];
    if (e.chipReg & 0x80) ayReset(a);
    else ayWrite(a, e.chipReg & 0x0F, e.value);
}

void frameBegin() {
    // Image précédente émulée sans son (disquette rapide, vitesse maximale) :
    // ses écritures doivent tout de même atteindre les puces
    for (int i = 0; i < eventCount; i++) applyEvent(events[i]);
    eventCount = 0;
    frameStart = cycles;
}

// Ajoute à `mix` (un entier par échantillon) le son des deux AY pour l'image
// qui vient d'être émulée. Rend faux si les deux puces sont muettes.
bool render(int32_t* mix, int count) {
    if (!enabled) { eventCount = 0; return false; }
    if (eventCount == 0 && aySilent(ay[0]) && aySilent(ay[1])) return false;

    // Horloge interne des AY : celle de l'Apple / 8, soit 17030 / 8 pas par image
    const uint32_t ticksPerSample = ((uint32_t)(A2_CYCLES_PER_FRAME / 8) << 16) / count;   // 16.16
    const uint32_t cyclesPerSample = ((uint32_t)A2_CYCLES_PER_FRAME << 12) / count;        // 20.12
    int ev = 0;
    uint32_t pos = 0;
    for (int i = 0; i < count; i++) {
        pos += cyclesPerSample;
        while (ev < eventCount && ((uint32_t)events[ev].time << 12) < pos) applyEvent(events[ev++]);
        tickFraction += ticksPerSample;
        int ticks = tickFraction >> 16;
        tickFraction &= 0xFFFF;
        uint32_t sum = 0;
        for (int t = 0; t < ticks; t++) sum += ayTick(ay[0]) + ayTick(ay[1]);
        // Moyenne sur les pas de l'échantillon : 6 voies de 0 à 255
        mix[i] += ticks ? (int32_t)(sum / ticks) : 0;
    }
    // Écritures tombées après le dernier échantillon
    for (; ev < eventCount; ev++) applyEvent(events[ev]);
    eventCount = 0;
    return true;
}

}
}
