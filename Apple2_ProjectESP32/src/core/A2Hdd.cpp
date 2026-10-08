/*

Apple2_ProjectESP32 — Disque dur ProDOS (slot 7).

Une carte fictive, comme en ont tous les émulateurs : son firmware de 256
octets, écrit pour ce projet, porte la signature d'un périphérique par blocs
ProDOS et se réduit à passer la main à l'émulateur. Une écriture en $C0F0
exécute la commande que ProDOS a préparée en page zéro ($42 : commande, $43 :
unité, $44-$45 : tampon, $46-$47 : numéro de bloc) ; le résultat se lit en
$C0F1 (code d'erreur) et $C0F2-$C0F3 (nombre de blocs, pour la commande d'état).

L'image est une suite de blocs de 512 octets (.hdv, .po, .2mg), de 800 Ko pour
une disquette 3,5 pouces à 32 Mo pour un volume ProDOS entier. Le slot 7 est le
premier que l'Apple essaie au démarrage : sans image, la carte est absente.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {
namespace Hdd {

uint8_t activity = 0;
uint32_t statReads = 0, statWrites = 0;

static uint32_t blocks = 0;           // 0 : pas de disque
static bool readOnly = false;
static uint8_t lastError = 0;

enum : uint8_t { ERR_NONE = 0x00, ERR_IO = 0x27, ERR_NO_DEVICE = 0x28, ERR_WRITE_PROTECTED = 0x2B };

// Firmware de la carte, en $C700
static const uint8_t firmware[] = {
    // Signature d'une carte de démarrage : $Cn01 = $20, $Cn03 = $00, $Cn05 = $03, $Cn07 = $3C
    0xA2, 0x20,             // C700  LDX #$20
    0xA9, 0x00,             // C702  LDA #$00
    0xA2, 0x03,             // C704  LDX #$03
    0xA9, 0x3C,             // C706  LDA #$3C
    // Démarrage : lit le bloc 0 de l'unité $70 en $0800 et lui passe la main
    0xA9, 0x01,             // C708  LDA #$01      commande : lecture
    0x85, 0x42,             // C70A  STA $42
    0xA9, 0x70,             // C70C  LDA #$70      slot 7, lecteur 1
    0x85, 0x43,             // C70E  STA $43
    0xA9, 0x00,             // C710  LDA #$00
    0x85, 0x44,             // C712  STA $44       tampon : $0800
    0x85, 0x46,             // C714  STA $46       bloc 0
    0x85, 0x47,             // C716  STA $47
    0xA9, 0x08,             // C718  LDA #$08
    0x85, 0x45,             // C71A  STA $45
    0x20, 0x30, 0xC7,       // C71C  JSR $C730
    0xB0, 0x05,             // C71F  BCS $C726
    0xA2, 0x70,             // C721  LDX #$70
    0x4C, 0x01, 0x08,       // C723  JMP $0801
    0x4C, 0x00, 0xC6,       // C726  JMP $C600     échec : démarrage sur la disquette
    0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA,       // C729
    // Pilote ProDOS
    0x8D, 0xF0, 0xC0,       // C730  STA $C0F0     exécute la commande
    0xAE, 0xF2, 0xC0,       // C733  LDX $C0F2
    0xAC, 0xF3, 0xC0,       // C736  LDY $C0F3
    0xAD, 0xF1, 0xC0,       // C739  LDA $C0F1     code d'erreur
    0xC9, 0x01,             // C73C  CMP #$01      retenue levée si erreur
    0x60,                   // C73E  RTS
};

static uint8_t rom[256];

void init() {
    memset(rom, 0, sizeof(rom));
    memcpy(rom, firmware, sizeof(firmware));
    rom[0xFC] = 0x00;       // nombre de blocs : à demander par la commande d'état
    rom[0xFD] = 0x00;
    rom[0xFE] = 0x07;       // un volume, non amovible ; lecture, écriture, état
    rom[0xFF] = 0x30;       // entrée du pilote : $C730
    blocks = 0;
}

const uint8_t* romPage() { return blocks ? rom : nullptr; }

bool inserted() { return blocks != 0; }

uint32_t blockCount() { return blocks; }

bool writeProtected() { return readOnly; }

void insert(uint32_t count, bool writeProtected) {
    blocks = count;
    readOnly = writeProtected;
    lastError = 0;
    Disk35::mediaChanged();
    slotsChanged();
}

void eject() {
    blocks = 0;
    Disk35::mediaChanged();
    slotsChanged();
}

// Exécute la commande ProDOS décrite en page zéro
static void execute() {
    uint8_t cmd = memRead(0x42);
    uint8_t unit = memRead(0x43);
    uint16_t buffer = memRead(0x44) | (memRead(0x45) << 8);
    uint32_t block = memRead(0x46) | (memRead(0x47) << 8);
    uint8_t data[512];

    lastError = ERR_NONE;
    // Un seul disque : le lecteur 2 du slot (bit 7 de l'unité) n'existe pas
    if (!blocks || (unit & 0x80)) { lastError = ERR_NO_DEVICE; return; }
    activity = 30;
    switch (cmd) {
        case 0:     // état : rien à faire, le nombre de blocs se lit en $C0F2-$C0F3
            break;
        case 1:     // lecture
            if (block >= blocks || !A2_platformHddRead(block, data)) { lastError = ERR_IO; break; }
            for (int i = 0; i < 512; i++) memWrite((uint16_t)(buffer + i), data[i]);
            statReads++;
            break;
        case 2:     // écriture
            if (readOnly) { lastError = ERR_WRITE_PROTECTED; break; }
            if (block >= blocks) { lastError = ERR_IO; break; }
            for (int i = 0; i < 512; i++) data[i] = memRead((uint16_t)(buffer + i));
            if (!A2_platformHddWrite(block, data)) lastError = ERR_IO;
            statWrites++;
            break;
        case 3:     // formatage : le volume est déjà une suite de blocs
            if (readOnly) lastError = ERR_WRITE_PROTECTED;
            break;
        default:
            lastError = ERR_IO;
            break;
    }
}

uint8_t ioRead(uint8_t reg) {
    switch (reg) {
        case 0x1: return lastError;
        case 0x2: return (uint8_t)(blocks & 0xFF);
        case 0x3: return (uint8_t)(blocks >> 8);
        default: return 0;
    }
}

void ioWrite(uint8_t reg, uint8_t value) {
    if (reg == 0x0) execute();
}

void frameTick() {
    if (activity) activity--;
}

}
}
