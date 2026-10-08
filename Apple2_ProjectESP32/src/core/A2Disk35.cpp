/*

Apple2_ProjectESP32 — Lecteur 3,5 pouces de 800 Ko (le lecteur interne de
l'Apple //c Plus).

C'est un lecteur « Sony » sans intelligence : le contrôleur IWM le commande
par quatre lignes. CA0, CA1, CA2 (les « phases » d'un lecteur 5,25 pouces) et
SEL (donnée par le circuit MIG) forment un numéro de registre. Ce numéro
choisit ce que le lecteur renvoie sur sa ligne d'état (disquette présente,
piste 0, protection...), et, sur un front montant de LSTRB (la quatrième
phase), la commande qu'il exécute (sens, pas, moteur, éjection).

La disquette a 80 pistes et deux faces ; les pistes extérieures portent plus
de secteurs (12, puis 11, 10, 9 et 8 par groupes de 16 pistes). Un secteur de
512 octets est précédé de 12 octets d'étiquette et codé en 6 et 2 avec une
somme de contrôle sur trois octets.

Le lecteur est émulé au niveau des octets lus par la tête, comme le lecteur
5,25 pouces : le secteur sous la tête est codé à la volée à partir du bloc de
l'image, et le disque attend le programme (un octet non lu n'est jamais
perdu). Ce qui est écrit est décodé au fil de l'eau : chaque champ de données
complet repart aussitôt dans l'image.

L'image est celle montée comme disque dur (800 Ko, ou 400 Ko pour une
disquette simple face), dans l'ordre des blocs ProDOS.

Commandes et états du lecteur repris de MAME (floppy.cpp, flopimg.cpp).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {
namespace Disk35 {

// Durée d'un octet sous la tête, en cycles. Un vrai lecteur en donne un toutes
// les 16 microsecondes, que le //c Plus lit avec son 65C02 accéléré à 4 MHz.
#define NIBBLE_CYCLES 16
// Sans nouvel octet à écrire pendant ce temps, l'IWM signale un sous-débit
#define UNDERRUN_CYCLES 400
// Il n'y a personne pour remettre une disquette éjectée : elle revient d'elle-même
// après ce délai, et le lecteur signale un changement de disquette
#define EJECT_CYCLES 3000000

#define TAG_SIZE 12
#define SECTOR_BYTES (TAG_SIZE + 512)
#define DATA_NIBBLES 699            // 174 triplets sur 4 octets, le dernier sur 3
#define FIELD_NIBBLES (1 + DATA_NIBBLES + 4)   // numéro de secteur, données, somme de contrôle

struct State {
    uint8_t cyl;                    // piste sous la tête (0 à 79)
    uint8_t sector;                 // secteur sous la tête
    bool stepOut;                   // sens du prochain pas : vers la piste 0
    bool motor;
    bool changed;                   // la disquette a été changée depuis le dernier acquittement
    bool ejected;                   // éjectée par le programme : absente un moment, puis remise
    uint32_t ejectCycles;
    bool side;                      // face choisie (ligne SEL)
    int16_t pos;                    // position dans le secteur
    uint32_t nibbleCycles;          // instant du dernier octet lu
    uint32_t writeCycles;           // instant du dernier octet écrit
    bool writing;
    // Ce qui s'écrit : 0 à 2, les marques D5 AA AD attendues ; 3, le champ de données
    uint8_t writeStage;
    int16_t writeCount;
};

static State s;
static bool present = false;        // la machine a ce lecteur

// Secteur sous la tête, tel que la tête le lit
static uint8_t buf[768];
static int bufLen = 0;
static int bufCyl = -1, bufSide = -1, bufSector = -1;
// Champ de données en cours d'écriture
static uint8_t field[FIELD_NIBBLES];

static const uint8_t gcr[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};
static uint8_t gcrInv[128];         // octet & 0x7F -> valeur sur 6 bits, 0xFF si invalide

static inline int sides() { return Hdd::blockCount() == 800 ? 1 : 2; }

static bool hasDisk() {
    uint32_t n = Hdd::blockCount();
    if (!present || (n != 1600 && n != 800)) return false;
    if (s.ejected) {
        if ((uint32_t)(cycles - s.ejectCycles) < EJECT_CYCLES) return false;
        s.ejected = false;
        s.changed = true;
    }
    return true;
}

static inline int sectorsOf(int cyl) { return 12 - cyl / 16; }

// Premier bloc d'une piste : les blocs se suivent piste par piste, face par
// face, secteur par secteur
static uint32_t firstBlock(int cyl, int side) {
    uint32_t n = 0;
    for (int zone = 0; zone < cyl / 16; zone++) n += 16 * (12 - zone);
    n += (uint32_t)(cyl % 16) * sectorsOf(cyl);
    return n * sides() + (side ? sectorsOf(cyl) : 0);
}

void init() {
    memset(gcrInv, 0xFF, sizeof(gcrInv));
    for (int i = 0; i < 64; i++) gcrInv[gcr[i] & 0x7F] = (uint8_t)i;
    memset(&s, 0, sizeof(s));
    bufCyl = -1;
}

void setPresent(bool on) {
    present = on;
    bufCyl = -1;
}

// Une image vient d'être montée ou retirée
void mediaChanged() {
    s.changed = true;
    s.ejected = false;
    s.writing = false;
    bufCyl = -1;
}

bool busy() { return hasDisk() && s.motor; }

// ---------------------------------------------------------------------------
// Codage et décodage d'un secteur
// ---------------------------------------------------------------------------

// 524 octets -> 699 octets codés et les 4 de la somme de contrôle
static uint8_t* encodeData(uint8_t* p, const uint8_t* data) {
    uint8_t ca = 0, cb = 0, cc = 0;
    for (int i = 0; i < 175; i++) {
        uint8_t va = data[3 * i], vb = data[3 * i + 1];
        uint8_t vc = i != 174 ? data[3 * i + 2] : 0;
        cc = (uint8_t)((cc << 1) | (cc >> 7));
        uint16_t sumA = (uint16_t)(ca + va + (cc & 1));
        ca = (uint8_t)sumA;
        va ^= cc;
        uint16_t sumB = (uint16_t)(cb + vb + (sumA >> 8));
        cb = (uint8_t)sumB;
        vb ^= ca;
        if (i != 174) cc = (uint8_t)(cc + vc + (sumB >> 8));
        vc ^= cb;
        *p++ = gcr[((va >> 2) & 0x30) | ((vb >> 4) & 0x0C) | ((vc >> 6) & 0x03)];
        *p++ = gcr[va & 0x3F];
        *p++ = gcr[vb & 0x3F];
        if (i != 174) *p++ = gcr[vc & 0x3F];
    }
    *p++ = gcr[((ca >> 2) & 0x30) | ((cb >> 4) & 0x0C) | ((cc >> 6) & 0x03)];
    *p++ = gcr[ca & 0x3F];
    *p++ = gcr[cb & 0x3F];
    *p++ = gcr[cc & 0x3F];
    return p;
}

// L'inverse. Faux si un octet ou la somme de contrôle est invalide.
static bool decodeData(const uint8_t* nib, uint8_t* data) {
    uint8_t ca = 0, cb = 0, cc = 0;
    for (int i = 0; i < 175; i++) {
        uint8_t high = gcrInv[*nib++ & 0x7F], ea = gcrInv[*nib++ & 0x7F], eb = gcrInv[*nib++ & 0x7F];
        uint8_t ec = i != 174 ? gcrInv[*nib++ & 0x7F] : 0;
        if ((high | ea | eb | ec) & 0xC0) return false;
        ea |= (high << 2) & 0xC0;
        eb |= (high << 4) & 0xC0;
        ec |= (high << 6) & 0xC0;
        cc = (uint8_t)((cc << 1) | (cc >> 7));
        uint8_t va = ea ^ cc;
        uint16_t sumA = (uint16_t)(ca + va + (cc & 1));
        ca = (uint8_t)sumA;
        uint8_t vb = eb ^ ca;
        uint16_t sumB = (uint16_t)(cb + vb + (sumA >> 8));
        cb = (uint8_t)sumB;
        data[3 * i] = va;
        data[3 * i + 1] = vb;
        if (i != 174) {
            uint8_t vc = ec ^ cb;
            cc = (uint8_t)(cc + vc + (sumB >> 8));
            data[3 * i + 2] = vc;
        }
    }
    uint8_t high = gcrInv[nib[0] & 0x7F], ea = gcrInv[nib[1] & 0x7F], eb = gcrInv[nib[2] & 0x7F], ec = gcrInv[nib[3] & 0x7F];
    if ((high | ea | eb | ec) & 0xC0) return false;
    ea |= (high << 2) & 0xC0;
    eb |= (high << 4) & 0xC0;
    ec |= (high << 6) & 0xC0;
    return ea == ca && eb == cb && ec == cc;
}

// Code le secteur sous la tête : synchronisation, champ d'adresse, champ de données
static void loadSector() {
    bufCyl = s.cyl;
    bufSide = s.side;
    bufSector = s.sector;
    uint8_t data[SECTOR_BYTES];
    memset(data, 0, TAG_SIZE);
    int side = (s.side && sides() == 2) ? 1 : 0;
    if (!A2_platformHddRead(firstBlock(s.cyl, side) + s.sector, data + TAG_SIZE)) memset(data + TAG_SIZE, 0, 512);
    Hdd::statReads++;
    uint8_t* p = buf;
    memset(p, 0xFF, 14);
    p += 14;
    uint8_t sideByte = (uint8_t)((s.cyl & 0x40 ? 1 : 0) | (side ? 0x20 : 0));
    uint8_t format = sides() == 2 ? 0x22 : 0x02;
    *p++ = 0xD5; *p++ = 0xAA; *p++ = 0x96;
    *p++ = gcr[s.cyl & 0x3F];
    *p++ = gcr[s.sector];
    *p++ = gcr[sideByte];
    *p++ = gcr[format];
    *p++ = gcr[((s.cyl & 0x3F) ^ s.sector ^ sideByte ^ format) & 0x3F];
    *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xFF;
    memset(p, 0xFF, 5);
    p += 5;
    *p++ = 0xD5; *p++ = 0xAA; *p++ = 0xAD;
    *p++ = gcr[s.sector];
    p = encodeData(p, data);
    *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xFF;
    bufLen = (int)(p - buf);
}

static inline void nextSector() {
    s.sector = (uint8_t)((s.sector + 1) % sectorsOf(s.cyl));
    s.pos = 0;
}

// ---------------------------------------------------------------------------
// Dialogue avec l'IWM
// ---------------------------------------------------------------------------

void setSide(bool side) { s.side = side; }

// Ligne d'état du lecteur pour le registre choisi (CA0, CA1, CA2 et SEL en bit 3)
bool sense(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0x0: return s.stepOut;                 // sens du pas
        case 0x1: return true;                      // pas terminé
        case 0x2: return !s.motor;                  // moteur arrêté
        case 0x3: return s.changed;                 // disquette changée
        case 0x6: return true;                      // lecteur double face
        case 0x7: return false;                     // un lecteur est là
        case 0x8: return !hasDisk();                // pas de disquette
        case 0x9: return !Hdd::writeProtected();    // disquette non protégée
        case 0xA: return s.cyl != 0;                // hors de la piste 0
        case 0xB: return ((cycles >> 9) & 1) != 0;  // tachymètre
        case 0xE: return !(s.motor && hasDisk());   // lecteur pas prêt
        case 0xF: return true;                      // lecteur de 800 Ko
        default:  return false;
    }
}

// Commande reçue sur un front montant de LSTRB
void command(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0x0: s.stepOut = false; break;
        case 0x4: s.stepOut = true; break;
        case 0x1:
            if (s.stepOut) { if (s.cyl > 0) s.cyl--; }
            else if (s.cyl < 79) s.cyl++;
            s.sector = (uint8_t)(s.sector % sectorsOf(s.cyl));
            s.pos = 0;
            break;
        case 0x2: s.motor = true; break;
        case 0x6: s.motor = false; break;
        case 0x7: s.ejected = true; s.ejectCycles = cycles; s.motor = false; break;
        case 0xC: s.changed = false; break;
        default: break;
    }
}

uint8_t readData() {
    if (!hasDisk() || !s.motor) return 0;
    if ((uint32_t)(cycles - s.nibbleCycles) < NIBBLE_CYCLES) return 0;
    s.nibbleCycles = cycles;
    if (bufLen && s.pos >= bufLen) nextSector();
    if (bufCyl != s.cyl || bufSide != (int)s.side || bufSector != s.sector) loadSector();
    Hdd::activity = 30;
    return buf[s.pos++];
}

// Un octet part vers la tête. Seuls les champs de données comptent : le
// formatage réécrit aussi les champs d'adresse, qui sont ici immuables.
void writeData(uint8_t v) {
    s.writing = true;
    s.writeCycles = cycles;
    if (!hasDisk() || !s.motor || Hdd::writeProtected()) return;
    Hdd::activity = 30;
    static const uint8_t mark[3] = { 0xD5, 0xAA, 0xAD };
    if (s.writeStage < 3) {
        if (v == mark[s.writeStage]) s.writeStage++;
        else s.writeStage = v == 0xD5 ? 1 : 0;
        s.writeCount = 0;
        return;
    }
    field[s.writeCount++] = v;
    if (s.writeCount < FIELD_NIBBLES) return;
    s.writeStage = 0;
    uint8_t data[SECTOR_BYTES];
    int sector = gcrInv[field[0] & 0x7F];
    if (sector < sectorsOf(s.cyl) && decodeData(field + 1, data)) {
        int side = (s.side && sides() == 2) ? 1 : 0;
        A2_platformHddWrite(firstBlock(s.cyl, side) + sector, data + TAG_SIZE);
        Hdd::statWrites++;
        // La tête est maintenant à la fin de ce secteur
        s.sector = (uint8_t)sector;
        s.pos = 0x7FFF;
        bufCyl = -1;
    }
}

void writeEnd() {
    s.writing = false;
    s.writeStage = 0;
}

// Registre de dialogue de l'IWM en écriture : bit 7, prêt pour l'octet
// suivant ; bit 6 à 0, plus rien à écrire depuis trop longtemps
uint8_t handshake() {
    bool underrun = s.writing && (uint32_t)(cycles - s.writeCycles) > UNDERRUN_CYCLES;
    return underrun ? 0xBF : 0xFF;
}

void state(StateIO& io) {
    io.bytes(&s, sizeof(s));
    if (!io.saving) {
        bufCyl = -1;
        s.writing = false;
        s.writeStage = 0;
    }
}

}
}
