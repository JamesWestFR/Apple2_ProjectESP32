/*

Apple2_ProjectESP32 — 6502 (NMOS, avec ses instructions non documentées stables)
et 65C02 (CMOS, celui du //e Enhanced, avec les instructions de bits Rockwell).

Le temps est compté par instruction : durée de base lue dans une table, plus
les cycles de franchissement de page et de branchement pris.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "A2.h"

namespace A2 {

CpuState cpu;
uint32_t cycles = 0;

static int32_t carryOver = 0;   // cycles exécutés en trop par l'appel précédent de cpuRun

enum : uint8_t { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };

static const uint8_t cycNmos[256] = {
    7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

static const uint8_t cycCmos[256] = {
    7,6,2,1,5,3,5,5,3,2,2,1,6,4,6,5,
    2,5,5,1,5,4,6,5,2,4,2,1,6,4,6,5,
    6,6,2,1,3,3,5,5,4,2,2,1,4,4,6,5,
    2,5,5,1,4,4,6,5,2,4,2,1,4,4,6,5,
    6,6,2,1,3,3,5,5,3,2,2,1,3,4,6,5,
    2,5,5,1,4,4,6,5,2,4,3,1,8,4,6,5,
    6,6,2,1,3,3,5,5,4,2,2,1,6,4,6,5,
    2,5,5,1,4,4,6,5,2,4,4,1,6,4,6,5,
    2,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5,
    2,6,5,1,4,4,4,5,2,5,2,1,4,5,5,5,
    2,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5,
    2,5,5,1,4,4,4,5,2,4,2,1,4,4,4,5,
    2,6,2,1,3,3,5,5,2,2,2,1,4,4,6,5,
    2,5,5,1,4,4,6,5,2,4,3,1,4,4,7,5,
    2,6,2,1,3,3,5,5,2,2,2,1,4,4,6,5,
    2,5,5,1,4,4,6,5,2,4,4,1,4,4,7,5,
};

// Longueur (en octets) d'une instruction non traitée explicitement : elle est
// exécutée comme un NOP de la bonne taille.
static int undefinedLength(uint8_t op, bool cmos) {
    if (cmos) {
        if ((op & 0x03) == 0x03) return 1;                 // colonnes 3 et B
        if (op == 0x5C || op == 0xDC || op == 0xFC) return 3;
        return 2;
    }
    switch (op & 0x1F) {
        case 0x02: return (op & 0x80) ? 2 : 1;             // immédiat ou KIL
        case 0x08: case 0x0A: case 0x12: case 0x18: case 0x1A: return 1;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x19: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: return 3;
        default: return 2;
    }
}

void cpuReset() {
    cpu.sp = 0xFD;
    cpu.p = (cpu.p | FI | FU) & ~(cpu.cmos ? FD : 0);
    cpu.pc = memRead(0xFFFC) | (memRead(0xFFFD) << 8);
    carryOver = 0;
}

#define RD(addr) memRead((uint16_t)(addr))
#define WR(addr, v) memWrite((uint16_t)(addr), (uint8_t)(v))
#define ZPG (rdPage[0])
#define PUSH(v) { wrPage[1][sp] = (uint8_t)(v); sp--; }
#define PULL() (rdPage[1][++sp])
#define SETNZ(v) { p = (p & ~(FN | FZ)) | ((v) & FN) | (((v) & 0xFF) ? 0 : FZ); }

// Modes d'adressage : calculent l'adresse effective `ea`
#define A_IMM  { ea = pc; pc++; }
#define A_ZP   { ea = RD(pc); pc++; }
#define A_ZPX  { ea = (uint8_t)(RD(pc) + x); pc++; }
#define A_ZPY  { ea = (uint8_t)(RD(pc) + y); pc++; }
#define A_ABS  { ea = RD(pc) | (RD(pc + 1) << 8); pc += 2; }
// Indexés : la variante R ajoute un cycle au franchissement de page (lecture)
#define A_ABXR { uint32_t b = RD(pc) | (RD(pc + 1) << 8); pc += 2; ea = (b + x) & 0xFFFF; cyc += ((b & 0xFF) + x) >> 8; }
#define A_ABXW { uint32_t b = RD(pc) | (RD(pc + 1) << 8); pc += 2; ea = (b + x) & 0xFFFF; }
#define A_ABYR { uint32_t b = RD(pc) | (RD(pc + 1) << 8); pc += 2; ea = (b + y) & 0xFFFF; cyc += ((b & 0xFF) + y) >> 8; }
#define A_ABYW { uint32_t b = RD(pc) | (RD(pc + 1) << 8); pc += 2; ea = (b + y) & 0xFFFF; }
#define A_IZX  { uint8_t z = (uint8_t)(RD(pc) + x); pc++; ea = ZPG[z] | (ZPG[(uint8_t)(z + 1)] << 8); }
#define A_IZYR { uint8_t z = RD(pc); pc++; uint32_t b = ZPG[z] | (ZPG[(uint8_t)(z + 1)] << 8); ea = (b + y) & 0xFFFF; cyc += ((b & 0xFF) + y) >> 8; }
#define A_IZYW { uint8_t z = RD(pc); pc++; uint32_t b = ZPG[z] | (ZPG[(uint8_t)(z + 1)] << 8); ea = (b + y) & 0xFFFF; }
#define A_IZP  { uint8_t z = RD(pc); pc++; ea = ZPG[z] | (ZPG[(uint8_t)(z + 1)] << 8); }

#define O_ORA { a |= RD(ea); SETNZ(a); }
#define O_AND { a &= RD(ea); SETNZ(a); }
#define O_EOR { a ^= RD(ea); SETNZ(a); }
#define O_LDA { a = RD(ea); SETNZ(a); }
#define O_LDX { x = RD(ea); SETNZ(x); }
#define O_LDY { y = RD(ea); SETNZ(y); }
#define O_CMP(reg) { uint32_t m = RD(ea); uint32_t r = (reg) - m; p = (p & ~(FN | FZ | FC)) | (r & FN) | ((r & 0xFF) ? 0 : FZ) | ((reg) >= m ? FC : 0); }
#define O_BIT { uint8_t m = RD(ea); p = (p & ~(FN | FV | FZ)) | (m & (FN | FV)) | ((a & m) ? 0 : FZ); }
#define O_ADC { uint8_t m = RD(ea); adc(m, a, p, cmos, cyc); }
#define O_SBC { uint8_t m = RD(ea); sbc(m, a, p, cmos, cyc); }

#define M_ASL(v) { p = (p & ~FC) | ((v) >> 7); v = (uint8_t)((v) << 1); SETNZ(v); }
#define M_LSR(v) { p = (p & ~FC) | ((v) & 1); v = (uint8_t)((v) >> 1); SETNZ(v); }
#define M_ROL(v) { uint8_t c = p & FC; p = (p & ~FC) | ((v) >> 7); v = (uint8_t)(((v) << 1) | c); SETNZ(v); }
#define M_ROR(v) { uint8_t c = p & FC; p = (p & ~FC) | ((v) & 1); v = (uint8_t)(((v) >> 1) | (c << 7)); SETNZ(v); }
#define M_INC(v) { v = (uint8_t)((v) + 1); SETNZ(v); }
#define M_DEC(v) { v = (uint8_t)((v) - 1); SETNZ(v); }
// Lecture-modification-écriture
#define RMW(OP) { uint8_t m = RD(ea); OP(m); WR(ea, m); }

#define BRANCH(cond) { int8_t off = (int8_t)RD(pc); pc++; if (cond) { uint16_t t = (uint16_t)(pc + off); cyc += 1 + (((t ^ pc) & 0xFF00) ? 1 : 0); pc = t; } }

static inline void adc(uint8_t m, uint8_t& a, uint8_t& p, bool cmos, int& cyc) {
    uint32_t c = p & FC;
    if (!(p & FD)) {
        uint32_t r = a + m + c;
        p = (p & ~(FC | FV | FN | FZ)) | (r > 0xFF ? FC : 0) | ((~(a ^ m) & (a ^ r) & 0x80) ? FV : 0)
          | (r & FN) | ((r & 0xFF) ? 0 : FZ);
        a = (uint8_t)r;
        return;
    }
    uint32_t bin = a + m + c;
    uint32_t lo = (a & 0x0F) + (m & 0x0F) + c;
    if (lo > 9) lo += 6;
    uint32_t hi = (a >> 4) + (m >> 4) + (lo > 0x0F ? 1 : 0);
    uint8_t flags = ((bin & 0xFF) ? 0 : FZ) | ((hi << 4) & FN) | ((~(a ^ m) & (a ^ (hi << 4)) & 0x80) ? FV : 0);
    if (hi > 9) hi += 6;
    if (hi > 0x0F) flags |= FC;
    a = (uint8_t)((hi << 4) | (lo & 0x0F));
    p = (p & ~(FC | FV | FN | FZ)) | flags;
    if (cmos) {
        // 65C02 : N et Z valides sur le résultat décimal, au prix d'un cycle
        p = (p & ~(FN | FZ)) | (a & FN) | (a ? 0 : FZ);
        cyc++;
    }
}

static inline void sbc(uint8_t m, uint8_t& a, uint8_t& p, bool cmos, int& cyc) {
    uint32_t c = p & FC;
    uint32_t r = a - m - (1 - c);
    uint8_t flags = ((r & 0x100) ? 0 : FC) | (((a ^ m) & (a ^ r) & 0x80) ? FV : 0) | (r & FN) | ((r & 0xFF) ? 0 : FZ);
    if (!(p & FD)) {
        a = (uint8_t)r;
        p = (p & ~(FC | FV | FN | FZ)) | flags;
        return;
    }
    int32_t lo = (a & 0x0F) - (m & 0x0F) - (int32_t)(1 - c);
    int32_t hi = (a >> 4) - (m >> 4);
    if (lo < 0) { lo -= 6; hi--; }
    if (hi < 0) hi -= 6;
    a = (uint8_t)((((uint32_t)hi & 0x0F) << 4) | ((uint32_t)lo & 0x0F));
    p = (p & ~(FC | FV | FN | FZ)) | flags;
    if (cmos) {
        p = (p & ~(FN | FZ)) | (a & FN) | (a ? 0 : FZ);
        cyc++;
    }
}

A2_FAST void cpuRun(int32_t budget) {
    uint16_t pc = cpu.pc;
    uint8_t a = cpu.a, x = cpu.x, y = cpu.y, sp = cpu.sp, p = cpu.p;
    const bool cmos = cpu.cmos;
    const uint8_t* cycTable = cmos ? cycCmos : cycNmos;
    int32_t remaining = budget - carryOver;

    while (remaining > 0) {
        if (timerArmed && (int32_t)(cycles - timerDue) >= 0) timerEvent();
        if (irqLine && !(p & FI)) {
            // Interruption : comme BRK, sans l'indicateur B
            PUSH(pc >> 8); PUSH(pc & 0xFF); PUSH((p & ~FB) | FU);
            p |= FI;
            if (cmos) p &= ~FD;
            pc = RD(0xFFFE) | (RD(0xFFFF) << 8);
            cycles += 7;
            remaining -= 7;
        }
        uint8_t op = RD(pc);
        pc++;
        int cyc = cycTable[op];
        // Les entrées/sorties voient le temps de la fin de l'instruction
        cycles += cyc;
        uint32_t ea;

        switch (op) {
            // --- Chargements, rangements ---
            case 0xA9: A_IMM;  O_LDA; break;
            case 0xA5: A_ZP;   O_LDA; break;
            case 0xB5: A_ZPX;  O_LDA; break;
            case 0xAD: A_ABS;  O_LDA; break;
            case 0xBD: A_ABXR; O_LDA; break;
            case 0xB9: A_ABYR; O_LDA; break;
            case 0xA1: A_IZX;  O_LDA; break;
            case 0xB1: A_IZYR; O_LDA; break;

            case 0xA2: A_IMM;  O_LDX; break;
            case 0xA6: A_ZP;   O_LDX; break;
            case 0xB6: A_ZPY;  O_LDX; break;
            case 0xAE: A_ABS;  O_LDX; break;
            case 0xBE: A_ABYR; O_LDX; break;

            case 0xA0: A_IMM;  O_LDY; break;
            case 0xA4: A_ZP;   O_LDY; break;
            case 0xB4: A_ZPX;  O_LDY; break;
            case 0xAC: A_ABS;  O_LDY; break;
            case 0xBC: A_ABXR; O_LDY; break;

            case 0x85: A_ZP;   WR(ea, a); break;
            case 0x95: A_ZPX;  WR(ea, a); break;
            case 0x8D: A_ABS;  WR(ea, a); break;
            case 0x9D: A_ABXW; WR(ea, a); break;
            case 0x99: A_ABYW; WR(ea, a); break;
            case 0x81: A_IZX;  WR(ea, a); break;
            case 0x91: A_IZYW; WR(ea, a); break;

            case 0x86: A_ZP;   WR(ea, x); break;
            case 0x96: A_ZPY;  WR(ea, x); break;
            case 0x8E: A_ABS;  WR(ea, x); break;

            case 0x84: A_ZP;   WR(ea, y); break;
            case 0x94: A_ZPX;  WR(ea, y); break;
            case 0x8C: A_ABS;  WR(ea, y); break;

            // --- Transferts, pile ---
            case 0xAA: x = a; SETNZ(x); break;
            case 0xA8: y = a; SETNZ(y); break;
            case 0x8A: a = x; SETNZ(a); break;
            case 0x98: a = y; SETNZ(a); break;
            case 0xBA: x = sp; SETNZ(x); break;
            case 0x9A: sp = x; break;
            case 0x48: PUSH(a); break;
            case 0x68: a = PULL(); SETNZ(a); break;
            case 0x08: PUSH(p | FB | FU); break;
            case 0x28: p = (PULL() & ~FB) | FU; break;

            // --- Logique ---
            case 0x09: A_IMM;  O_ORA; break;
            case 0x05: A_ZP;   O_ORA; break;
            case 0x15: A_ZPX;  O_ORA; break;
            case 0x0D: A_ABS;  O_ORA; break;
            case 0x1D: A_ABXR; O_ORA; break;
            case 0x19: A_ABYR; O_ORA; break;
            case 0x01: A_IZX;  O_ORA; break;
            case 0x11: A_IZYR; O_ORA; break;

            case 0x29: A_IMM;  O_AND; break;
            case 0x25: A_ZP;   O_AND; break;
            case 0x35: A_ZPX;  O_AND; break;
            case 0x2D: A_ABS;  O_AND; break;
            case 0x3D: A_ABXR; O_AND; break;
            case 0x39: A_ABYR; O_AND; break;
            case 0x21: A_IZX;  O_AND; break;
            case 0x31: A_IZYR; O_AND; break;

            case 0x49: A_IMM;  O_EOR; break;
            case 0x45: A_ZP;   O_EOR; break;
            case 0x55: A_ZPX;  O_EOR; break;
            case 0x4D: A_ABS;  O_EOR; break;
            case 0x5D: A_ABXR; O_EOR; break;
            case 0x59: A_ABYR; O_EOR; break;
            case 0x41: A_IZX;  O_EOR; break;
            case 0x51: A_IZYR; O_EOR; break;

            case 0x24: A_ZP;   O_BIT; break;
            case 0x2C: A_ABS;  O_BIT; break;

            // --- Arithmétique ---
            case 0x69: A_IMM;  O_ADC; break;
            case 0x65: A_ZP;   O_ADC; break;
            case 0x75: A_ZPX;  O_ADC; break;
            case 0x6D: A_ABS;  O_ADC; break;
            case 0x7D: A_ABXR; O_ADC; break;
            case 0x79: A_ABYR; O_ADC; break;
            case 0x61: A_IZX;  O_ADC; break;
            case 0x71: A_IZYR; O_ADC; break;

            case 0xE9: A_IMM;  O_SBC; break;
            case 0xE5: A_ZP;   O_SBC; break;
            case 0xF5: A_ZPX;  O_SBC; break;
            case 0xED: A_ABS;  O_SBC; break;
            case 0xFD: A_ABXR; O_SBC; break;
            case 0xF9: A_ABYR; O_SBC; break;
            case 0xE1: A_IZX;  O_SBC; break;
            case 0xF1: A_IZYR; O_SBC; break;

            case 0xC9: A_IMM;  O_CMP(a); break;
            case 0xC5: A_ZP;   O_CMP(a); break;
            case 0xD5: A_ZPX;  O_CMP(a); break;
            case 0xCD: A_ABS;  O_CMP(a); break;
            case 0xDD: A_ABXR; O_CMP(a); break;
            case 0xD9: A_ABYR; O_CMP(a); break;
            case 0xC1: A_IZX;  O_CMP(a); break;
            case 0xD1: A_IZYR; O_CMP(a); break;

            case 0xE0: A_IMM;  O_CMP(x); break;
            case 0xE4: A_ZP;   O_CMP(x); break;
            case 0xEC: A_ABS;  O_CMP(x); break;
            case 0xC0: A_IMM;  O_CMP(y); break;
            case 0xC4: A_ZP;   O_CMP(y); break;
            case 0xCC: A_ABS;  O_CMP(y); break;

            // --- Incréments, décalages ---
            case 0xE6: A_ZP;   RMW(M_INC); break;
            case 0xF6: A_ZPX;  RMW(M_INC); break;
            case 0xEE: A_ABS;  RMW(M_INC); break;
            case 0xFE: A_ABXW; RMW(M_INC); break;
            case 0xC6: A_ZP;   RMW(M_DEC); break;
            case 0xD6: A_ZPX;  RMW(M_DEC); break;
            case 0xCE: A_ABS;  RMW(M_DEC); break;
            case 0xDE: A_ABXW; RMW(M_DEC); break;
            case 0xE8: M_INC(x); break;
            case 0xC8: M_INC(y); break;
            case 0xCA: M_DEC(x); break;
            case 0x88: M_DEC(y); break;

            // Sur le 65C02, les décalages en absolu,X coûtent 6 cycles, 7 si la page change
            case 0x0A: M_ASL(a); break;
            case 0x06: A_ZP;   RMW(M_ASL); break;
            case 0x16: A_ZPX;  RMW(M_ASL); break;
            case 0x0E: A_ABS;  RMW(M_ASL); break;
            case 0x1E: if (cmos) A_ABXR else A_ABXW; RMW(M_ASL); break;
            case 0x4A: M_LSR(a); break;
            case 0x46: A_ZP;   RMW(M_LSR); break;
            case 0x56: A_ZPX;  RMW(M_LSR); break;
            case 0x4E: A_ABS;  RMW(M_LSR); break;
            case 0x5E: if (cmos) A_ABXR else A_ABXW; RMW(M_LSR); break;
            case 0x2A: M_ROL(a); break;
            case 0x26: A_ZP;   RMW(M_ROL); break;
            case 0x36: A_ZPX;  RMW(M_ROL); break;
            case 0x2E: A_ABS;  RMW(M_ROL); break;
            case 0x3E: if (cmos) A_ABXR else A_ABXW; RMW(M_ROL); break;
            case 0x6A: M_ROR(a); break;
            case 0x66: A_ZP;   RMW(M_ROR); break;
            case 0x76: A_ZPX;  RMW(M_ROR); break;
            case 0x6E: A_ABS;  RMW(M_ROR); break;
            case 0x7E: if (cmos) A_ABXR else A_ABXW; RMW(M_ROR); break;

            // --- Sauts, branchements ---
            case 0x4C: pc = RD(pc) | (RD(pc + 1) << 8); break;
            case 0x6C: {
                uint16_t ptr = RD(pc) | (RD(pc + 1) << 8);
                // 6502 : l'octet haut est lu sans retenue sur la page ($xxFF -> $xx00)
                uint16_t hiAddr = cmos ? (uint16_t)(ptr + 1) : (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF));
                pc = RD(ptr) | (RD(hiAddr) << 8);
                break;
            }
            case 0x20: {
                uint16_t target = RD(pc) | (RD(pc + 1) << 8);
                pc++;
                PUSH(pc >> 8); PUSH(pc & 0xFF);
                pc = target;
                break;
            }
            case 0x60: { uint8_t lo = PULL(); uint8_t hi = PULL(); pc = (uint16_t)((lo | (hi << 8)) + 1); break; }
            case 0x40: { p = (PULL() & ~FB) | FU; uint8_t lo = PULL(); uint8_t hi = PULL(); pc = lo | (hi << 8); break; }
            case 0x00:
                pc++;
                PUSH(pc >> 8); PUSH(pc & 0xFF); PUSH(p | FB | FU);
                p |= FI;
                if (cmos) p &= ~FD;
                pc = RD(0xFFFE) | (RD(0xFFFF) << 8);
                break;

            case 0x10: BRANCH(!(p & FN)); break;
            case 0x30: BRANCH(p & FN); break;
            case 0x50: BRANCH(!(p & FV)); break;
            case 0x70: BRANCH(p & FV); break;
            case 0x90: BRANCH(!(p & FC)); break;
            case 0xB0: BRANCH(p & FC); break;
            case 0xD0: BRANCH(!(p & FZ)); break;
            case 0xF0: BRANCH(p & FZ); break;

            // --- Indicateurs ---
            case 0x18: p &= ~FC; break;
            case 0x38: p |= FC; break;
            case 0x58: p &= ~FI; break;
            case 0x78: p |= FI; break;
            case 0xB8: p &= ~FV; break;
            case 0xD8: p &= ~FD; break;
            case 0xF8: p |= FD; break;
            case 0xEA: break;

            default:
                if (cmos) {
                    // --- Ajouts du 65C02 ---
                    switch (op) {
                        case 0x12: A_IZP; O_ORA; break;
                        case 0x32: A_IZP; O_AND; break;
                        case 0x52: A_IZP; O_EOR; break;
                        case 0x72: A_IZP; O_ADC; break;
                        case 0x92: A_IZP; WR(ea, a); break;
                        case 0xB2: A_IZP; O_LDA; break;
                        case 0xD2: A_IZP; O_CMP(a); break;
                        case 0xF2: A_IZP; O_SBC; break;
                        case 0x1A: M_INC(a); break;
                        case 0x3A: M_DEC(a); break;
                        case 0x5A: PUSH(y); break;
                        case 0x7A: y = PULL(); SETNZ(y); break;
                        case 0xDA: PUSH(x); break;
                        case 0xFA: x = PULL(); SETNZ(x); break;
                        case 0x64: A_ZP;   WR(ea, 0); break;
                        case 0x74: A_ZPX;  WR(ea, 0); break;
                        case 0x9C: A_ABS;  WR(ea, 0); break;
                        case 0x9E: A_ABXW; WR(ea, 0); break;
                        case 0x34: A_ZPX;  O_BIT; break;
                        case 0x3C: A_ABXR; O_BIT; break;
                        case 0x89: { A_IMM; p = (p & ~FZ) | ((a & RD(ea)) ? 0 : FZ); break; }
                        case 0x04: { A_ZP;  uint8_t m = RD(ea); p = (p & ~FZ) | ((a & m) ? 0 : FZ); WR(ea, m | a); break; }
                        case 0x0C: { A_ABS; uint8_t m = RD(ea); p = (p & ~FZ) | ((a & m) ? 0 : FZ); WR(ea, m | a); break; }
                        case 0x14: { A_ZP;  uint8_t m = RD(ea); p = (p & ~FZ) | ((a & m) ? 0 : FZ); WR(ea, m & ~a); break; }
                        case 0x1C: { A_ABS; uint8_t m = RD(ea); p = (p & ~FZ) | ((a & m) ? 0 : FZ); WR(ea, m & ~a); break; }
                        case 0x80: BRANCH(true); break;
                        case 0x7C: { uint16_t ptr = (uint16_t)((RD(pc) | (RD(pc + 1) << 8)) + x); pc = RD(ptr) | (RD(ptr + 1) << 8); break; }
                        default:
                            if ((op & 0x0F) == 0x07) {
                                // RMB / SMB (65C02 Rockwell et WDC) : met un bit de la page zéro à 0 ou à 1
                                A_ZP;
                                uint8_t m = RD(ea), bit = (uint8_t)(1 << ((op >> 4) & 7));
                                WR(ea, (op & 0x80) ? (m | bit) : (m & ~bit));
                            } else if ((op & 0x0F) == 0x0F) {
                                // BBR / BBS : branchement selon un bit de la page zéro
                                A_ZP;
                                bool set = (RD(ea) >> ((op >> 4) & 7)) & 1;
                                BRANCH(set == ((op & 0x80) != 0));
                            } else {
                                pc += undefinedLength(op, true) - 1;
                            }
                            break;
                    }
                } else {
                    // --- Instructions non documentées du 6502 NMOS ---
                    switch (op) {
#define U_SLO { uint8_t m = RD(ea); M_ASL(m); WR(ea, m); a |= m; SETNZ(a); }
#define U_RLA { uint8_t m = RD(ea); M_ROL(m); WR(ea, m); a &= m; SETNZ(a); }
#define U_SRE { uint8_t m = RD(ea); M_LSR(m); WR(ea, m); a ^= m; SETNZ(a); }
#define U_RRA { uint8_t m = RD(ea); M_ROR(m); WR(ea, m); adc(m, a, p, false, cyc); }
#define U_DCP { uint8_t m = (uint8_t)(RD(ea) - 1); WR(ea, m); uint32_t r = a - m; p = (p & ~(FN | FZ | FC)) | (r & FN) | ((r & 0xFF) ? 0 : FZ) | (a >= m ? FC : 0); }
#define U_ISC { uint8_t m = (uint8_t)(RD(ea) + 1); WR(ea, m); sbc(m, a, p, false, cyc); }
#define U_LAX { a = x = RD(ea); SETNZ(a); }
#define U_FAMILY(base, OP) \
                        case base + 0x03: A_IZX;  OP; break; \
                        case base + 0x07: A_ZP;   OP; break; \
                        case base + 0x0F: A_ABS;  OP; break; \
                        case base + 0x13: A_IZYW; OP; break; \
                        case base + 0x17: A_ZPX;  OP; break; \
                        case base + 0x1B: A_ABYW; OP; break; \
                        case base + 0x1F: A_ABXW; OP; break;
                        U_FAMILY(0x00, U_SLO)
                        U_FAMILY(0x20, U_RLA)
                        U_FAMILY(0x40, U_SRE)
                        U_FAMILY(0x60, U_RRA)
                        U_FAMILY(0xC0, U_DCP)
                        U_FAMILY(0xE0, U_ISC)
                        case 0x83: A_IZX; WR(ea, a & x); break;
                        case 0x87: A_ZP;  WR(ea, a & x); break;
                        case 0x8F: A_ABS; WR(ea, a & x); break;
                        case 0x97: A_ZPY; WR(ea, a & x); break;
                        case 0xA3: A_IZX;  U_LAX; break;
                        case 0xA7: A_ZP;   U_LAX; break;
                        case 0xAF: A_ABS;  U_LAX; break;
                        case 0xB3: A_IZYR; U_LAX; break;
                        case 0xB7: A_ZPY;  U_LAX; break;
                        case 0xBF: A_ABYR; U_LAX; break;
                        case 0x0B: case 0x2B: A_IMM; a &= RD(ea); SETNZ(a); p = (p & ~FC) | (a >> 7); break;      // ANC
                        case 0x4B: A_IMM; a &= RD(ea); M_LSR(a); break;                                            // ALR
                        case 0x6B: {                                                                               // ARR
                            A_IMM;
                            a &= RD(ea);
                            a = (uint8_t)((a >> 1) | ((p & FC) << 7));
                            SETNZ(a);
                            p = (p & ~(FC | FV)) | ((a >> 6) & FC) | (((a >> 6) ^ (a >> 5)) & 1 ? FV : 0);
                            break;
                        }
                        case 0xCB: {                                                                               // SBX
                            A_IMM;
                            uint32_t m = RD(ea), v = a & x;
                            p = (p & ~FC) | (v >= m ? FC : 0);
                            x = (uint8_t)(v - m);
                            SETNZ(x);
                            break;
                        }
                        case 0xEB: A_IMM; O_SBC; break;
                        // NOP à opérande : la lecture a lieu (elle peut toucher un soft switch)
                        case 0x04: case 0x44: case 0x64: A_ZP; (void)RD(ea); break;
                        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: A_ZPX; (void)RD(ea); break;
                        case 0x0C: A_ABS; (void)RD(ea); break;
                        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: A_ABXR; (void)RD(ea); break;
                        default: pc += undefinedLength(op, false) - 1; break;
                    }
                }
                break;
        }

        // Cycles ajoutés en cours d'instruction (page franchie, branchement pris)
        cycles += cyc - cycTable[op];
        remaining -= cyc;
    }

    carryOver = -remaining;
    cpu.pc = pc;
    cpu.a = a; cpu.x = x; cpu.y = y; cpu.sp = sp; cpu.p = p;
}

}
