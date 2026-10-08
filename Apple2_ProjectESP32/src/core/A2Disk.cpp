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

#include <stdlib.h>
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
// Contrôleur IWM du //c : compatible avec la carte Disk II, plus un registre de
// mode que sa ROM écrit puis relit dans le registre d'état
static bool iwm = false;
static uint8_t iwmMode = 0;
#define NIBBLE_CYCLES 32

// Piste sous la tête
static uint8_t trackBuf[A2_NIB_TRACK_SIZE];
static int restorePos = -1;         // position à reprendre après une restauration d'état
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

IdentifyResult identify(const char* ext, uint32_t fileSize, const uint8_t* head, uint32_t headLen,
                        Format* fmt, int* tracks, uint32_t* dataOffset) {
    uint32_t size = fileSize, offset = 0;
    Format f = FMT_DOS;
    if (headLen >= 256 && (memcmp(head, "WOZ1", 4) == 0 || memcmp(head, "WOZ2", 4) == 0)) {
        // WOZ : `dataOffset` porte la version du format (1 ou 2), `tracks` les
        // 160 quarts de piste de sa table
        if (head[21] != 1) return ID_BAD_SIZE;      // disquette 3,5 pouces
        *fmt = FMT_WOZ;
        *tracks = 160;
        *dataOffset = head[3] - '0';
        return ID_OK;
    }
    if (headLen >= 64 && memcmp(head, "2IMG", 4) == 0) {
        // 2MG : en-tête de 64 octets, format et position des données
        uint32_t imgFormat = head[12] | (head[13] << 8);
        offset = head[24] | (head[25] << 8) | (head[26] << 16) | ((uint32_t)head[27] << 24);
        uint32_t length = head[28] | (head[29] << 8) | (head[30] << 16) | ((uint32_t)head[31] << 24);
        if (offset < 64 || offset >= fileSize) return ID_BAD_2MG;
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
        if (size % 512 || size / 512 > 65535) return ID_BAD_SIZE;
        *fmt = FMT_HDD;
        *tracks = (int)(size / 512);
        *dataOffset = offset;
        return ID_OK;
    }
    uint32_t trackSize = f == FMT_NIB ? A2_NIB_TRACK_SIZE : TRACK_BYTES;
    uint32_t count = size / trackSize;
    if (count < 34 || count > 40 || size % trackSize) return ID_BAD_SIZE;
    *fmt = f;
    *tracks = (int)count;
    *dataOffset = offset;
    return ID_OK;
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

void setIwm(bool on) {
    iwm = on;
    iwmMode = 0;
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

// Image WOZ : la piste est un flux de bits tel que la tête le lit. Il est
// converti en octets comme le fait la carte Disk II : les bits entrent dans un
// registre à décalage, et un octet est complet quand son bit de poids fort
// vaut 1 (les zéros en trop des octets de synchronisation disparaissent
// d'eux-mêmes). Les quarts de piste de l'image sont respectés ; les
// protections qui mesurent des durées ne le sont pas.
static void loadWozTrack(int drive, int quarter) {
    Drive& d = drives[drive];
    trackLen = 6400;
    memset(trackBuf, 0xFF, trackLen);       // piste absente : rien de lisible
    uint8_t index = 0xFF;
    if (quarter >= 160 || !A2_platformDiskRead(drive, 88 + quarter, &index, 1) || index == 0xFF) return;

    uint32_t start, bits;
    uint8_t t[8];
    if (d.dataOffset == 2) {
        if (!A2_platformDiskRead(drive, 256 + 8 * (uint32_t)index, t, 8)) return;
        start = (uint32_t)(t[0] | (t[1] << 8)) * 512;
        bits = t[4] | (t[5] << 8) | (t[6] << 16) | ((uint32_t)t[7] << 24);
    } else {
        start = 256 + (uint32_t)index * 6656;
        if (!A2_platformDiskRead(drive, start + 6646, t, 4)) return;
        bits = t[2] | (t[3] << 8);
    }
    uint32_t bytes = (bits + 7) / 8;
    if (bits < 64 || bytes > 2 * A2_NIB_TRACK_SIZE) return;
    uint8_t* raw = (uint8_t*)malloc(bytes);
    if (!raw) return;
    if (A2_platformDiskRead(drive, start, raw, bytes)) {
        uint8_t reg = 0;
        int n = 0;
        // Un premier passage sur la fin de la piste cale le registre : la piste est un anneau
        for (uint32_t i = bits - 64; i < bits; i++) {
            reg = (uint8_t)((reg << 1) | ((raw[i >> 3] >> (7 - (i & 7))) & 1));
            if (reg & 0x80) reg = 0;
        }
        for (uint32_t i = 0; i < bits && n < A2_NIB_TRACK_SIZE; i++) {
            reg = (uint8_t)((reg << 1) | ((raw[i >> 3] >> (7 - (i & 7))) & 1));
            if (reg & 0x80) {
                trackBuf[n++] = reg;
                reg = 0;
            }
        }
        if (n > 0) trackLen = n;
        statReads++;
    }
    free(raw);
}

static void loadTrack(int drive, int track) {
    Drive& d = drives[drive];
    trackDrive = drive;
    trackNum = track;
    trackDirty = false;
    trackPos = 0;
    trackLen = 0;
    if (d.fmt == FMT_NONE) return;

    if (d.fmt == FMT_WOZ) {
        loadWozTrack(drive, track);
        return;
    }

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

void state(StateIO& io) {
    if (io.saving) flush();
    for (int d = 0; d < 2; d++) io.value(drives[d].quarterTrack);
    io.value(cur); io.value(phases); io.value(motorOn); io.value(motorStopping); io.value(motorOffCycles);
    io.value(q6); io.value(q7); io.value(latch); io.value(nibbleCycles);
    int pos = trackPos;
    io.value(pos);
    if (!io.saving) {
        // La piste sera reconvertie à la prochaine lecture, à la même position
        trackDrive = trackNum = -1;
        trackLen = 0;
        trackDirty = false;
        restorePos = pos;
    }
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
                // Seuls les secteurs qui ont changé sont écrits dans l'image
                uint8_t old[SECTOR_SIZE];
                if (!A2_platformDiskRead(trackDrive, offset, old, SECTOR_SIZE) || memcmp(old, sector, SECTOR_SIZE) != 0)
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
    // Une image WOZ décrit chaque quart de piste ; les autres, les pistes entières
    int track = drives[cur].fmt == FMT_WOZ ? drives[cur].quarterTrack : drives[cur].quarterTrack >> 2;
    if (trackDrive != cur || trackNum != track) {
        flush();
        loadTrack(cur, track);
        if (restorePos >= 0) {
            if (restorePos < trackLen) trackPos = restorePos;
            restorePos = -1;
        }
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
    drives[drive].writeProtected = writeProtected || fmt == FMT_WOZ;
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
            // Fin d'une écriture : le secteur part aussitôt dans l'image, sans
            // attendre l'arrêt du moteur (une coupure de courant ne le perd plus)
            if (q7 && trackDirty) flush();
            q7 = false;
            // Q6 haut, Q7 bas : lecture de la protection en écriture
            if (q6) latch = (drives[cur].fmt != FMT_NONE && drives[cur].writeProtected) ? 0xFF : 0x00;
            // IWM : registre d'état (protection, moteur, mode)
            if (q6 && iwm) latch = (latch & 0x80) | (motorOn ? 0x20 : 0) | iwmMode;
            break;
        case 0xF:
            q7 = true;
            if (isWrite) {
                latch = value;
                // IWM : moteur arrêté, Q6 et Q7 hauts, l'écriture va au registre de mode
                if (iwm && q6 && !motorOn) iwmMode = value & 0x1F;
            }
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
