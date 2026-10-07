/*

Apple2_ProjectESP32 — Carte Disk II (slot 6), deux lecteurs 5,25 pouces.

Le lecteur est émulé au niveau des « nibbles » : la piste sous la tête est
convertie à la volée en la suite d'octets qu'on lirait sur une vraie disquette
(champs d'adresse et de données en codage 6 et 2). Un octet se présente tous
les 32 cycles, comme sur un vrai lecteur ; une lecture faite avant rend le
registre avec son bit 7 à 0 (le DOS compare deux lectures rapprochées pour
savoir si le disque tourne). En revanche le disque attend le programme : un
octet non lu n'est jamais perdu, ce qui rend la lecture insensible aux petits
écarts de durée de l'émulation. Ce modèle convient aux images .dsk, .do, .po
et .nib ; il ne permet pas les protections qui mesurent des durées.

Une piste modifiée est décodée et réécrite dans l'image quand la tête change
de piste, quand le lecteur change ou quand le moteur s'arrête.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {
namespace Disk {

uint8_t activity[2] = { 0, 0 };
uint32_t statReads = 0, statWrites = 0;

struct Drive {
    Format fmt;
    uint8_t tracks;
    bool writeProtected;
    int16_t quarterTrack;
    uint32_t dataOffset;       // position des données dans le fichier (en-tête 2MG)
};

static Drive drives[2];
static int cur = 0;                 // lecteur sélectionné
static uint8_t phases = 0;          // électro-aimants du moteur pas à pas
static bool motorOn = false;
static bool motorStopping = false;  // moteur coupé, encore lancé pendant une seconde
static uint32_t motorOffCycles = 0;
static bool q6 = false, q7 = false;
static uint8_t latch = 0;
static uint32_t nibbleCycles = 0;   // instant où le dernier octet lu s'est présenté
#define NIBBLE_CYCLES 32

// Piste sous la tête
static uint8_t trackBuf[A2_NIB_TRACK_SIZE];
static int trackLen = 0;
static int trackPos = 0;
static int trackDrive = -1, trackNum = -1;
static bool trackDirty = false;

#define VOLUME 254
#define SECTORS 16
#define SECTOR_SIZE 256
#define TRACK_BYTES (SECTORS * SECTOR_SIZE)
#define MOTOR_OFF_DELAY 1000000     // une seconde de 6502

static const uint8_t sixAndTwo[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};
static uint8_t sixAndTwoInv[128];   // nibble & 0x7F -> valeur sur 6 bits, 0xFF si invalide

// Secteur physique -> secteur dans le fichier (entrelacement DOS 3.3 ou ProDOS)
static inline int fileSector(Format fmt, int physical) {
    if (physical == 15) return 15;
    return (physical * (fmt == FMT_PRODOS ? 8 : 7)) % 15;
}

// Moteur pas à pas, au quart de piste (tables d'apple2ts). Chaque combinaison
// d'aimants attire la tête vers une des 8 positions d'un cycle de deux pistes.
static const int8_t magnetToPosition[16] = { -1, 0, 2, 1, 4, -1, 3, -1, 6, 7, -1, -1, 5, -1, -1, -1 };
static const int8_t positionToDirection[8][8] = {
    {  0,  1,  2,  3,  0, -3, -2, -1 },
    { -1,  0,  1,  2,  3,  0, -3, -2 },
    { -2, -1,  0,  1,  2,  3,  0, -3 },
    { -3, -2, -1,  0,  1,  2,  3,  0 },
    {  0, -3, -2, -1,  0,  1,  2,  3 },
    {  3,  0, -3, -2, -1,  0,  1,  2 },
    {  2,  3,  0, -3, -2, -1,  0,  1 },
    {  1,  2,  3,  0, -3, -2, -1,  0 },
};

void init() {
    memset(sixAndTwoInv, 0xFF, sizeof(sixAndTwoInv));
    for (int i = 0; i < 64; i++) sixAndTwoInv[sixAndTwo[i] & 0x7F] = (uint8_t)i;
    for (int d = 0; d < 2; d++) {
        drives[d].fmt = FMT_NONE;
        drives[d].quarterTrack = 0;
    }
    cur = 0;
    phases = 0;
    motorOn = motorStopping = false;
    q6 = q7 = false;
    trackDrive = trackNum = -1;
    trackLen = 0;
    trackDirty = false;
}

const char* identify(const char* ext, uint32_t fileSize, const uint8_t* head, uint32_t headLen,
                     Format* fmt, int* tracks, uint32_t* dataOffset) {
    uint32_t size = fileSize, offset = 0;
    Format f = FMT_DOS;
    if (headLen >= 64 && memcmp(head, "2IMG", 4) == 0) {
        // 2MG : en-tête de 64 octets, format et position des données
        uint32_t imgFormat = head[12] | (head[13] << 8);
        offset = head[24] | (head[25] << 8) | (head[26] << 16) | ((uint32_t)head[27] << 24);
        uint32_t length = head[28] | (head[29] << 8) | (head[30] << 16) | ((uint32_t)head[31] << 24);
        if (offset < 64 || offset >= fileSize) return "En-tete 2MG invalide";
        size = (length && offset + length <= fileSize) ? length : fileSize - offset;
        f = imgFormat == 0 ? FMT_DOS : imgFormat == 1 ? FMT_PRODOS : FMT_NIB;
    } else if (strcmp(ext, "po") == 0) {
        f = FMT_PRODOS;
    } else if (strcmp(ext, "nib") == 0) {
        f = FMT_NIB;
    } else if (strcmp(ext, "do") != 0 && headLen >= 0x406) {
        // Un .dsk est parfois dans l'ordre ProDOS : le bloc 2 (répertoire du
        // volume) est alors à la position 1024, sans bloc précédent et avec
        // un en-tête de volume
        if (head[0x400] == 0 && head[0x401] == 0 && (head[0x404] >> 4) == 0x0F && (head[0x404] & 0x0F) != 0)
            f = FMT_PRODOS;
    }
    if (f != FMT_NIB && size >= 400 * 1024) {
        // Trop grand pour une disquette 5,25 pouces : un volume ProDOS par blocs
        if (size % 512 || size / 512 > 65535) return "Taille d'image non reconnue";
        *fmt = FMT_HDD;
        *tracks = (int)(size / 512);
        *dataOffset = offset;
        return nullptr;
    }
    uint32_t trackSize = f == FMT_NIB ? A2_NIB_TRACK_SIZE : TRACK_BYTES;
    uint32_t count = size / trackSize;
    if (count < 34 || count > 40 || size % trackSize) return "Taille d'image non reconnue";
    *fmt = f;
    *tracks = (int)count;
    *dataOffset = offset;
    return nullptr;
}

int quarterTrack(int drive) { return drives[drive & 1].quarterTrack; }

bool spinning() {
    if (motorOn) return true;
    if (motorStopping) {
        if ((uint32_t)(cycles - motorOffCycles) < MOTOR_OFF_DELAY) return true;
        motorStopping = false;
    }
    return false;
}

bool busy() { return spinning() && drives[cur].fmt != FMT_NONE; }

// Le signal RESET de l'Apple remet à zéro le registre de la carte : moteur
// arrêté, aimants coupés, mode lecture
void reset() {
    flush();
    motorOn = motorStopping = false;
    phases = 0;
    q6 = q7 = false;
}

// ---------------------------------------------------------------------------
// Codage et décodage d'une piste
// ---------------------------------------------------------------------------

static inline uint8_t* put44(uint8_t* p, uint8_t v) {
    *p++ = (v >> 1) | 0xAA;
    *p++ = v | 0xAA;
    return p;
}

// 256 octets -> 343 nibbles (342 de données et la somme de contrôle)
static uint8_t* encodeSector(uint8_t* p, const uint8_t* src) {
    static const uint8_t rev2[4] = { 0, 2, 1, 3 };
    uint8_t v[342];
    for (int c = 0; c < 86; c++) {
        uint8_t b = rev2[src[c] & 3] | (rev2[src[c + 86] & 3] << 2);
        if (c < 84) b |= rev2[src[c + 172] & 3] << 4;
        v[c] = b;
    }
    for (int c = 0; c < 256; c++) v[86 + c] = src[c] >> 2;
    uint8_t prev = 0;
    for (int c = 0; c < 342; c++) {
        *p++ = sixAndTwo[v[c] ^ prev];
        prev = v[c];
    }
    *p++ = sixAndTwo[prev];
    return p;
}

// 343 nibbles -> 256 octets. Faux si un nibble ou la somme de contrôle est invalide.
static bool decodeSector(const uint8_t* nib, int pos, uint8_t* dst) {
    static const uint8_t rev2[4] = { 0, 2, 1, 3 };
    uint8_t v[342];
    uint8_t prev = 0;
    for (int c = 0; c < 342; c++) {
        uint8_t d = sixAndTwoInv[nib[pos] & 0x7F];
        if (d == 0xFF) return false;
        prev ^= d;
        v[c] = prev;
        if (++pos >= trackLen) pos = 0;
    }
    if (sixAndTwoInv[nib[pos] & 0x7F] != prev) return false;
    for (int c = 0; c < 256; c++)
        dst[c] = (uint8_t)((v[86 + c] << 2) | rev2[(v[c % 86] >> (2 * (c / 86))) & 3]);
    return true;
}

static void loadTrack(int drive, int track) {
    Drive& d = drives[drive];
    trackDrive = drive;
    trackNum = track;
    trackDirty = false;
    trackPos = 0;
    trackLen = 0;
    if (d.fmt == FMT_NONE) return;

    if (d.fmt == FMT_NIB) {
        trackLen = A2_NIB_TRACK_SIZE;
        if (track >= d.tracks
            || !A2_platformDiskRead(drive, d.dataOffset + (uint32_t)track * A2_NIB_TRACK_SIZE, trackBuf, A2_NIB_TRACK_SIZE))
            memset(trackBuf, 0xFF, A2_NIB_TRACK_SIZE);
        return;
    }

    uint8_t* p = trackBuf;
    if (track >= d.tracks) {
        // Au-delà de la dernière piste de l'image : surface non formatée
        memset(trackBuf, 0xFF, 6384);
        trackLen = 6384;
        return;
    }
    uint8_t sector[SECTOR_SIZE];
    memset(p, 0xFF, 48);
    p += 48;
    for (int phys = 0; phys < SECTORS; phys++) {
        // Champ d'adresse
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0x96;
        p = put44(p, VOLUME);
        p = put44(p, (uint8_t)track);
        p = put44(p, (uint8_t)phys);
        p = put44(p, (uint8_t)(VOLUME ^ track ^ phys));
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        memset(p, 0xFF, 6);
        p += 6;
        // Champ de données
        uint32_t offset = d.dataOffset + (uint32_t)track * TRACK_BYTES + (uint32_t)fileSector(d.fmt, phys) * SECTOR_SIZE;
        if (!A2_platformDiskRead(drive, offset, sector, SECTOR_SIZE)) memset(sector, 0, SECTOR_SIZE);
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0xAD;
        p = encodeSector(p, sector);
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        memset(p, 0xFF, 27);
        p += 27;
    }
    trackLen = (int)(p - trackBuf);
    statReads++;
}

// Relit dans le tampon de piste les secteurs qu'un programme vient d'écrire
static void storeTrack() {
    Drive& d = drives[trackDrive];
    if (d.fmt == FMT_NIB) {
        A2_platformDiskWrite(trackDrive, d.dataOffset + (uint32_t)trackNum * A2_NIB_TRACK_SIZE, trackBuf, A2_NIB_TRACK_SIZE);
        return;
    }
    uint8_t sector[SECTOR_SIZE];
    uint16_t done = 0;
    #define NIB(i) trackBuf[(i) % trackLen]
    // Les accès bouclent sur le tampon : un champ peut être à cheval sur sa fin
    for (int i = 0; i < trackLen && done != 0xFFFF; i++) {
        if (NIB(i) != 0xD5 || NIB(i + 1) != 0xAA || NIB(i + 2) != 0x96) continue;
        int phys = ((NIB(i + 7) << 1) | 1) & NIB(i + 8);
        if (phys >= SECTORS || (done & (1 << phys))) continue;
        // Le champ de données suit de près son champ d'adresse
        for (int j = i + 11; j < i + 11 + 40; j++) {
            if (NIB(j) != 0xD5 || NIB(j + 1) != 0xAA || NIB(j + 2) != 0xAD) continue;
            if (decodeSector(trackBuf, (j + 3) % trackLen, sector)) {
                uint32_t offset = d.dataOffset + (uint32_t)trackNum * TRACK_BYTES
                                + (uint32_t)fileSector(d.fmt, phys) * SECTOR_SIZE;
                A2_platformDiskWrite(trackDrive, offset, sector, SECTOR_SIZE);
                done |= 1 << phys;
            }
            break;
        }
    }
    #undef NIB
}

void flush() {
    if (!trackDirty) return;
    trackDirty = false;
    if (trackDrive < 0 || trackNum < 0 || trackLen == 0) return;
    if (drives[trackDrive].fmt == FMT_NONE || trackNum >= drives[trackDrive].tracks) return;
    storeTrack();
    statWrites++;
}

static inline void ensureTrack() {
    int track = drives[cur].quarterTrack >> 2;
    if (trackDrive != cur || trackNum != track) {
        flush();
        loadTrack(cur, track);
    }
}

void insert(int drive, Format fmt, int tracks, uint32_t dataOffset, bool writeProtected) {
    drive &= 1;
    if (trackDrive == drive) {
        flush();
        trackDrive = trackNum = -1;
    }
    drives[drive].fmt = fmt;
    drives[drive].tracks = (uint8_t)tracks;
    drives[drive].dataOffset = dataOffset;
    drives[drive].writeProtected = writeProtected;
}

void eject(int drive) {
    drive &= 1;
    if (trackDrive == drive) {
        flush();
        trackDrive = trackNum = -1;
        trackLen = 0;
    }
    drives[drive].fmt = FMT_NONE;
}

void frameTick() {
    for (int d = 0; d < 2; d++)
        if (activity[d]) activity[d]--;
    if (trackDirty && !spinning()) flush();
}

// ---------------------------------------------------------------------------
// Registres $C0E0-$C0EF
// ---------------------------------------------------------------------------

static void step(uint8_t reg) {
    if (reg & 1) phases |= 1 << (reg >> 1);
    else phases &= ~(1 << (reg >> 1));
    int position = magnetToPosition[phases];
    if (position < 0 || !spinning()) return;
    Drive& d = drives[cur];
    int q = d.quarterTrack + positionToDirection[d.quarterTrack & 7][position];
    // 40 pistes au plus : la tête bute en 0 et après la dernière
    if (q < 0) q = 0;
    if (q > 39 * 4) q = 39 * 4;
    d.quarterTrack = (int16_t)q;
}

uint8_t access(uint8_t reg, bool isWrite, uint8_t value) {
    switch (reg) {
        case 0x8:
            if (motorOn) {
                motorOn = false;
                motorStopping = true;
                motorOffCycles = cycles;
            }
            break;
        case 0x9:
            motorOn = true;
            motorStopping = false;
            break;
        case 0xA:
        case 0xB:
            cur = reg & 1;
            break;
        case 0xC:
            q6 = false;
            if (!spinning() || drives[cur].fmt == FMT_NONE) break;
            ensureTrack();
            if (trackLen == 0) break;
            activity[cur] = 30;
            if (!q7) {
                uint32_t elapsed = cycles - nibbleCycles;
                if (elapsed < NIBBLE_CYCLES) {
                    // L'octet suivant n'est pas encore entièrement passé sous la tête
                    latch &= 0x7F;
                    break;
                }
                // Garde la cadence moyenne de 32 cycles tant que le programme suit
                nibbleCycles = elapsed < 2 * NIBBLE_CYCLES ? nibbleCycles + NIBBLE_CYCLES : cycles;
                latch = trackBuf[trackPos];
            } else if (!drives[cur].writeProtected) {
                trackBuf[trackPos] = latch;
                trackDirty = true;
            }
            if (++trackPos >= trackLen) trackPos = 0;
            break;
        case 0xD:
            q6 = true;
            if (isWrite) latch = value;
            break;
        case 0xE:
            q7 = false;
            // Q6 haut, Q7 bas : lecture de la protection en écriture
            if (q6) latch = (drives[cur].fmt != FMT_NONE && drives[cur].writeProtected) ? 0xFF : 0x00;
            break;
        case 0xF:
            q7 = true;
            if (isWrite) latch = value;
            break;
        default:
            step(reg);
            break;
    }
    // Le registre de données n'est sur le bus qu'aux adresses paires
    return (reg & 1) ? videoFloatingBus() : latch;
}

}
}
