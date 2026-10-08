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
#include "roms_apple2.h"

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
// //c Plus : le circuit MIG aiguille le lecteur 2 vers le lecteur 3,5 pouces
// interne, ou les deux vers des lecteurs 3,5 pouces externes (absents ici)
static bool plus = false;
static bool migInternal = false, migExternal35 = false, migSel = false;
// Bus SmartPort : émission de l'Apple en cours
static uint32_t busWriteCycles = 0;
static bool busWriting = false;
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

// Lecteur que l'IWM adresse sur un //c Plus
enum { DEV_525 = 0, DEV_35_INTERNAL, DEV_35_ABSENT };
static inline int device() {
    if (!plus) return DEV_525;
    if (cur == 1 && migInternal) return DEV_35_INTERNAL;
    return migExternal35 ? DEV_35_ABSENT : DEV_525;
}

void setPlus(bool on) {
    plus = on;
    migInternal = migExternal35 = migSel = false;
}

void setMig(bool internalDrive, bool external35, bool sel) {
    migInternal = internalDrive;
    migExternal35 = external35;
    migSel = sel;
    Disk35::setSide(sel);
}

bool busy() {
    if (device() == DEV_35_INTERNAL) return motorOn && Disk35::busy();
    // Bus SmartPort ouvert, IWM en service : un échange avec le disque externe
    if (iwm && motorOn && (phases & 0x0A) == 0x0A) return true;
    return spinning() && drives[cur].fmt != FMT_NONE;
}

// Le signal RESET de l'Apple remet à zéro le registre de la carte : moteur
// arrêté, aimants coupés, mode lecture
void reset() {
    flush();
    Disk35::reset();
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

// Image WOZ : la piste est un flux de bits tel que la tête le lit, gardé tel
// quel. Le disque tourne avec le temps du 6502, qu'on le lise ou non : les
// protections qui comptent les octets de synchronisation, mesurent la longueur
// d'une piste ou attendent un passage précis se comportent comme sur un vrai
// lecteur. Les quarts de piste de l'image sont respectés.
//
// Les bits sont remis au vrai séquenceur de la carte : la machine à états de
// la PROM P6 (341-0028), exécutée à 2 MHz, deux pas par cycle de 6502, comme
// le font MAME et pom2. Chaque bit à 1 est une impulsion au milieu de sa
// cellule ; la PROM décide à chaque pas de décaler, d'effacer ou de garder le
// registre de données. La durée de présentation d'un octet, sa remise à zéro
// et la lecture de la protection en écriture (Q6) sont donc celles du matériel.
static uint32_t wozBits = 0;            // longueur de la piste, en bits
static uint32_t wozPos = 0;             // bit qui suivra celui sous la tête
static uint32_t wozCycles = 0;          // instant de la position
static uint32_t wozPhase = 0;           // temps passé dans le bit sous la tête (unités de WOZ_TICK)
static uint8_t wozBit = 0;              // bit sous la tête, bruit compris
static bool wozPulsed = false;          // ce bit a déjà donné son impulsion
static uint8_t wozTiming = 32;          // durée d'un bit, en pas de 125 ns (32 : 4 microsecondes)
static uint8_t lssAddress = 0x10;       // état du séquenceur : adresse présentée à la PROM
static uint8_t lssData = 0;             // registre de données de la carte
// Un pas du séquenceur dure un demi-cycle de 6502. Les durées d'une image WOZ
// sont comptées comme sur les disquettes écrites par un Apple : un bit de
// « 4 microsecondes » (32 pas de 125 ns) y vaut 4 cycles, donc un demi-cycle
// vaut 4 pas, ici en 16.16. Le lecteur tourne 0,3 % plus vite que ce nominal :
// aucun vrai lecteur n'est exactement à 300 tours par minute, et sans cet
// écart le disque et le 6502 restent en phase au cycle près. Une boucle de
// lecture trop juste, qui échoue sur un calage et recommence (le chargeur de
// Wasteland), retomberait alors indéfiniment sur le même.
#define WOZ_TICK 262930u
// Après une longue absence de lecture, seuls les derniers pas comptent pour le registre
#define WOZ_MAX_TICKS 800u
static uint8_t wozWindow = 0;           // 4 derniers bits lus (bits faibles)
static uint32_t wozRandom = 0x2A2A2A2A;

static inline uint32_t wozRandomBit() {
    wozRandom = wozRandom * 1664525u + 1013904223u;
    // Environ un bit à 1 sur trois, comme le bruit d'une piste vide
    return ((wozRandom >> 24) % 10) < 3;
}

static void loadWozTrack(int drive, int quarter) {
    Drive& d = drives[drive];
    uint32_t oldBits = wozBits;
    // Piste absente de l'image : du bruit
    wozBits = 0;
    // WOZ2 : durée d'un bit propre à la disquette
    uint8_t timing = 32;
    if (d.dataOffset == 2 && A2_platformDiskRead(drive, 59, &timing, 1) && timing >= 24 && timing <= 40) wozTiming = timing;
    else wozTiming = 32;
    trackLen = A2_NIB_TRACK_SIZE;
    uint8_t index = 0xFF;
    if (quarter < 160 && A2_platformDiskRead(drive, 88 + quarter, &index, 1) && index != 0xFF) {
        uint32_t start, bits;
        uint8_t t[8];
        bool ok;
        if (d.dataOffset == 2) {
            ok = A2_platformDiskRead(drive, 256 + 8 * (uint32_t)index, t, 8);
            start = (uint32_t)(t[0] | (t[1] << 8)) * 512;
            bits = t[4] | (t[5] << 8) | (t[6] << 16) | ((uint32_t)t[7] << 24);
        } else {
            start = 256 + (uint32_t)index * 6656;
            ok = A2_platformDiskRead(drive, start + 6646, t, 4);
            bits = t[2] | (t[3] << 8);
        }
        uint32_t bytes = (bits + 7) / 8;
        if (ok && bits >= 64 && bytes <= A2_NIB_TRACK_SIZE && A2_platformDiskRead(drive, start, trackBuf, bytes)) {
            wozBits = bits;
            statReads++;
        }
    }
    // La tête garde sa position angulaire en changeant de piste
    if (wozBits) wozPos = oldBits ? (uint32_t)((uint64_t)wozPos * wozBits / oldBits) % wozBits : wozPos % wozBits;
}

// Bit suivant sous la tête
static inline void wozNextBit() {
    uint32_t bit;
    if (wozBits) {
        bit = (trackBuf[wozPos >> 3] >> (7 - (wozPos & 7))) & 1;
        if (++wozPos >= wozBits) wozPos = 0;
        // Plus de trois zéros de suite : l'amplificateur de lecture rend du bruit
        wozWindow = (uint8_t)(((wozWindow << 1) | bit) & 0x0F);
        if (wozWindow == 0) bit = wozRandomBit();
    } else {
        bit = wozRandomBit();
    }
    wozBit = (uint8_t)bit;
}

// Fait tourner le disque et le séquenceur jusqu'à l'instant présent, et rend
// le registre de données
A2_FAST static uint8_t wozRead() {
    uint32_t elapsed = cycles - wozCycles;
    wozCycles = cycles;
    // Un moteur resté longtemps sans lecture : le nombre de tours faits importe peu
    if (elapsed > 4000000) elapsed = 4000000;
    uint32_t ticks = elapsed * 2;
    const uint32_t cell = (uint32_t)wozTiming << 16, half = cell >> 1;
    if (ticks > WOZ_MAX_TICKS) {
        uint64_t t = (uint64_t)(ticks - WOZ_MAX_TICKS) * WOZ_TICK + wozPhase;
        uint32_t skipped = (uint32_t)(t / cell);
        wozPhase = (uint32_t)(t % cell);
        if (skipped) {
            if (wozBits) wozPos = (wozPos + skipped - 1) % wozBits;
            wozNextBit();
            wozPulsed = wozPhase >= half;
        }
        ticks = WOZ_MAX_TICKS;
    }
    uint32_t phase = wozPhase;
    uint8_t address = lssAddress, data = lssData;
    bool pulsed = wozPulsed;
    const uint8_t protect = drives[cur].writeProtected ? 0x80 : 0x00;
    while (ticks--) {
        phase += WOZ_TICK;
        address |= 0x10;
        if (!pulsed && phase >= half) {
            // Milieu de la cellule : l'impulsion d'un bit à 1, pendant un pas
            pulsed = true;
            if (wozBit) address &= ~0x10;
        }
        if (phase >= cell) {
            phase -= cell;
            pulsed = false;
            wozNextBit();
        }
        uint8_t op = gb_rom_disk2_p6[address];
        address = (uint8_t)((address & 0x1E) | (op & 0xC0) | ((op & 0x20) >> 5) | ((op & 0x10) << 1));
        switch (op & 0x0F) {
            case 0x8: case 0xC: break;
            case 0x9: data = (uint8_t)(data << 1); break;
            case 0xA: case 0xE: data = (uint8_t)((data >> 1) | protect); break;
            case 0xB: case 0xF: data = latch; break;
            case 0xD: data = (uint8_t)((data << 1) | 1); break;
            default: data = 0; break;
        }
        // QA : le bit 7 du registre revient à la PROM
        if (data & 0x80) address |= 0x02; else address &= ~0x02;
    }
    wozPhase = phase;
    wozPulsed = pulsed;
    lssAddress = address;
    lssData = data;
    return data;
}

// Q6 et Q7 viennent de changer : la PROM les reçoit sur son adresse
static inline void wozSwitches() {
    lssAddress = (uint8_t)((lssAddress & ~0x0C) | (q6 ? 0x04 : 0) | (q7 ? 0x08 : 0));
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
    // Lecteur WOZ : position de la tête et état du séquenceur, pour qu'une
    // reprise en plein chargement retrouve le disque au même angle
    io.value(wozPos); io.value(wozCycles); io.value(wozPhase); io.value(wozBit); io.value(wozPulsed);
    io.value(wozWindow); io.value(wozRandom); io.value(lssAddress); io.value(lssData); io.value(iwmMode);
    if (!io.saving) {
        // La piste sera reconvertie à la prochaine lecture, à la même position
        trackDrive = trackNum = -1;
        trackLen = 0;
        trackDirty = false;
        restorePos = pos;
        // Sans longueur de piste connue, loadWozTrack garde la position telle quelle
        wozBits = 0;
        busWriting = false;
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

// //c Plus, lecteur 3,5 pouces choisi : les phases deviennent le numéro de
// registre du lecteur et sa ligne de commande, et l'IWM donne ses registres à
// toutes les adresses selon Q6 et Q7
static uint8_t access35(int dev, uint8_t reg, bool isWrite, uint8_t value) {
    bool internal = dev == DEV_35_INTERNAL;
    switch (reg) {
        case 0x8:
            if (q7 && internal) Disk35::writeEnd();
            motorOn = false;
            break;
        case 0x9: motorOn = true; break;
        case 0xA: case 0xB: break;      // lecteur déjà choisi par l'appelant
        case 0xC: q6 = false; break;
        case 0xD: q6 = true; break;
        case 0xE:
            if (q7 && internal) Disk35::writeEnd();
            q7 = false;
            break;
        case 0xF: q7 = true; break;
        default: {
            uint8_t old = phases;
            if (reg & 1) phases |= 1 << (reg >> 1);
            else phases &= ~(1 << (reg >> 1));
            // Front montant de LSTRB : la commande du registre choisi
            if (internal && motorOn && (phases & 8) && !(old & 8)) Disk35::command((phases & 7) | (migSel ? 8 : 0));
            break;
        }
    }
    if (isWrite && (reg & 1) && q6 && q7) {
        if (!motorOn) iwmMode = value & 0x1F;
        else if (internal) Disk35::writeData(value);
    }
    if (q6 && q7) return 0xFF;
    if (q7) return internal ? Disk35::handshake() : 0xFF;
    if (q6) {
        // Registre d'état : ligne d'état du lecteur (haute sans lecteur), moteur, mode
        bool sense = !(internal && motorOn) || Disk35::sense((phases & 7) | (migSel ? 8 : 0));
        return (sense ? 0x80 : 0) | (motorOn ? 0x20 : 0) | iwmMode;
    }
    if (!motorOn) return 0xFF;
    return internal ? Disk35::readData() : 0;
}

// //c : les phases 1 et 3 ensemble font de la prise du lecteur externe un bus
// SmartPort. Les lecteurs 5,25 pouces s'effacent et l'IWM échange des paquets
// avec les périphériques du bus (A2SmartPort.cpp). Sans périphérique, la ligne
// d'état reste haute et le paquet du firmware reste sans accusé de réception.
#define BUS_UNDERRUN_CYCLES 64      // deux durées d'octet sans rien à émettre

static uint8_t accessBus(uint8_t reg, bool isWrite, uint8_t value) {
    switch (reg) {
        case 0x8: motorOn = false; break;
        case 0x9: motorOn = true; break;
        case 0xA: case 0xB: cur = reg & 1; break;
        case 0xC: q6 = false; break;
        case 0xD: q6 = true; break;
        case 0xE: q7 = false; busWriting = false; break;
        case 0xF: q7 = true; break;
        default:
            if (reg & 1) phases |= 1 << (reg >> 1);
            else phases &= ~(1 << (reg >> 1));
            SmartPort::phases(phases);
            break;
    }
    if (isWrite && (reg & 1) && q6 && q7) {
        if (!motorOn) iwmMode = value & 0x1F;
        else {
            busWriting = true;
            busWriteCycles = cycles;
            SmartPort::write(value);
        }
    }
    if (q6 && q7) return 0xFF;
    // Registre de dialogue : prêt pour l'octet suivant ; bit 6 à 0 quand l'émission est finie
    if (q7) return (busWriting && (uint32_t)(cycles - busWriteCycles) > BUS_UNDERRUN_CYCLES) ? 0xBF : 0xFF;
    if (q6) return (SmartPort::ack() ? 0x80 : 0) | (motorOn ? 0x20 : 0) | iwmMode;
    return motorOn ? SmartPort::read() : 0xFF;
}

uint8_t access(uint8_t reg, bool isWrite, uint8_t value) {
    if (iwm) {
        uint8_t p = phases;
        if (reg < 8) {
            if (reg & 1) p |= 1 << (reg >> 1);
            else p &= ~(1 << (reg >> 1));
        }
        if (reg < 8 && device() == DEV_525) {
            if ((p & 0x0F) == 0x05) SmartPort::busReset();
            if ((p & 0x0A) != 0x0A) SmartPort::phases(0);
        }
        if ((p & 0x0A) == 0x0A && device() == DEV_525) return accessBus(reg, isWrite, value);
    }
    if (plus) {
        if (reg == 0xA || reg == 0xB) cur = reg & 1;
        int dev = device();
        if (dev != DEV_525) return access35(dev, reg, isWrite, value);
    }
    switch (reg) {
        case 0x8:
            if (motorOn) {
                motorOn = false;
                motorStopping = true;
                motorOffCycles = cycles;
            }
            break;
        case 0x9:
            // Un disque à l'arrêt ne tourne pas : le temps repart d'ici
            if (!spinning()) wozCycles = cycles;
            motorOn = true;
            motorStopping = false;
            break;
        case 0xA:
        case 0xB:
            cur = reg & 1;
            break;
        case 0xC:
            if (!spinning()) { q6 = false; break; }
            if (drives[cur].fmt == FMT_NONE) {
                // Lecteur vide : l'amplificateur de lecture rend du bruit, et le
                // registre continue de se remplir. Un registre figé, bit 7 à 0,
                // bloquerait à jamais la boucle d'attente d'un programme qui
                // s'adresse au lecteur 2 (constat repris de pom2).
                q6 = false;
                if (q7) break;
                if ((uint32_t)(cycles - nibbleCycles) < NIBBLE_CYCLES) latch &= 0x7F;
                else {
                    nibbleCycles = cycles;
                    wozRandom = wozRandom * 1664525u + 1013904223u;
                    latch = (uint8_t)(0x80 | (wozRandom >> 24));
                }
                break;
            }
            ensureTrack();
            if (trackLen == 0) { q6 = false; break; }
            activity[cur] = 30;
            if (drives[cur].fmt == FMT_WOZ) {
                // Lecture au bit près ; une image WOZ ne s'écrit pas. Le temps
                // écoulé l'a été avec les bascules d'avant cet accès.
                uint8_t v = wozRead();
                q6 = false;
                wozSwitches();
                if (!q7) latch = v;
                break;
            }
            q6 = false;
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
            if (drives[cur].fmt == FMT_WOZ && spinning()) {
                // La tête a pu changer de piste ou de lecteur depuis la dernière lecture
                ensureTrack();
                wozRead();
            }
            q6 = true;
            if (isWrite) latch = value;
            wozSwitches();
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
