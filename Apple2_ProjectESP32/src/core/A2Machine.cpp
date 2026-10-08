/*

Apple2_ProjectESP32 — Mémoire, soft switches, clavier, manettes, haut-parleur
et boucle d'une image.

Organisation de la RAM (deux blocs de 64 Ko, principal et auxiliaire) :
  $0000-$BFFF   à leur adresse ;
  $C000-$CFFF   banque 1 de la carte langage (lue en $D000-$DFFF) : ces 4 Ko
                n'existent pas en RAM à cette adresse, la place est libre ;
  $D000-$FFFF   banque 2 et $E000-$FFFF de la carte langage.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"
#include "roms_apple2.h"

namespace A2 {

uint8_t* rdPage[256];
uint8_t* wrPage[256];
uint32_t sw = 0;
uint32_t frameCount = 0;
uint32_t powerOnCount = 0;
int scanline = 0;
bool renderEnabled = true;

static uint8_t* mainRam = nullptr;
static uint8_t* auxRam = nullptr;
static Model curModel = MODEL_IIE_ENH;
static bool iie = true;              // //e, //e Enhanced ou //c
static bool iic = false;             // //c : pas de slots, tout $C100-$CFFF est sa ROM
static const uint8_t* romBase = gb_rom_apple2e_enh;   // début de l'image de ROM du modèle
static const uint8_t* rom = gb_rom_apple2e_enh;   // //e : $C000-$FFFF ; ][ et ][+ : $D000-$FFFF
static uint8_t sink[256];                         // reçoit les écritures en ROM

// Clavier
static uint8_t keyLatch = 0;
static bool anyKeyDown = false;

// Manettes
static bool buttons[3] = { false, false, false };
static uint8_t paddles[4] = { 127, 127, 127, 127 };
static uint32_t paddleStart = 0;

// Haut-parleur : instants (en cycles depuis le début de l'image) des bascules
#define SPK_MAX_EVENTS 2048
static uint16_t spkEvents[SPK_MAX_EVENTS];
static int spkCount = 0;
static bool spkLevel = false;        // niveau courant
static bool spkFrameLevel = false;   // niveau au début de l'image rendue par renderAudio
static uint32_t frameStartCycles = 0;
static uint32_t lineStartCycles = 0;

uint8_t* ramMain() { return mainRam; }
uint8_t* ramAux() { return auxRam; }

const char* modelName(Model m) {
    static const char* names[MODEL_COUNT] = { "Apple ][", "Apple ][+", "Apple //e", "Apple //e Enhanced", "Apple //c",
                                               "Apple //c ROM 0", "Apple //c ROM 3", "Apple //c ROM 4",
                                               "Apple //c Plus" };
    return m < MODEL_COUNT ? names[m] : "?";
}

Model model() { return curModel; }
bool isIIe() { return iie; }
bool isIIc() { return iic; }

// ---------------------------------------------------------------------------
// Tables de pages
// ---------------------------------------------------------------------------

static inline bool hasAux() { return iie && auxRam != nullptr; }

// $0200-$BFFF : RAMRD / RAMWRT, sauf les pages vidéo quand 80STORE est actif
static void pageRange(int first, int last) {
    bool aux = hasAux();
    uint8_t* r = (aux && (sw & SW_RAMRD)) ? auxRam : mainRam;
    uint8_t* w = (aux && (sw & SW_RAMWRT)) ? auxRam : mainRam;
    uint8_t* video = (aux && (sw & SW_PAGE2)) ? auxRam : mainRam;
    bool store80 = aux && (sw & SW_80STORE);
    for (int page = first; page <= last; page++) {
        bool videoPage = store80 && ((page >= 0x04 && page <= 0x07)
                                  || ((sw & SW_HIRES) && page >= 0x20 && page <= 0x3F));
        rdPage[page] = (videoPage ? video : r) + (page << 8);
        wrPage[page] = (videoPage ? video : w) + (page << 8);
    }
}

// Page zéro, pile et $D000-$FFFF : ALTZP choisit le bloc, la carte langage la suite
static void pageZpAndLc() {
    uint8_t* base = (hasAux() && (sw & SW_ALTZP)) ? auxRam : mainRam;
    rdPage[0] = wrPage[0] = base;
    rdPage[1] = wrPage[1] = base + 0x100;
    for (int page = 0xD0; page <= 0xFF; page++) {
        uint8_t* ram = base + (((page < 0xE0 && !(sw & SW_LCBANK2)) ? page - 0x10 : page) << 8);
        const uint8_t* romPage = rom + ((page - (iie ? 0xC0 : 0xD0)) << 8);
        rdPage[page] = (sw & SW_LCRAM) ? ram : (uint8_t*)romPage;
        wrPage[page] = (sw & SW_LCWRITE) ? ram : sink;
    }
}

// $C100-$CEFF : les pages dont le contenu est une ROM connue sont lues
// directement (le 6502 y exécute le firmware du //e et celui de la carte
// disque). Restent sur le chemin lent, par ioRead : $C0xx (entrées/sorties),
// $CFxx (un accès à $CFFF rend $C800-$CFFF aux cartes), la page $C3 tant que
// son accès doit activer la ROM interne, et les pages sans ROM (bus flottant).
static void pageSlots() {
    for (int page = 0xC1; page <= 0xCE; page++) {
        const uint8_t* direct = nullptr;
        if (iic || (iie && (sw & SW_INTCXROM))) direct = rom + ((page - 0xC0) << 8);
        else if (page == 0xC6) direct = gb_rom_disk2;
        else if (page == 0xC7) direct = Hdd::romPage();
        else if (page == 0xC2) direct = Mouse::romPage();
        else if (iie && page >= 0xC8 && (sw & SW_INTC8ROM)) direct = rom + ((page - 0xC0) << 8);
        rdPage[page] = (uint8_t*)direct;
        // Une écriture en ROM est sans effet ; les registres de la Mockingboard
        // ($C400) passent par ioWrite
        bool internalRom = iic || (iie && (sw & SW_INTCXROM));
        wrPage[page] = (page == 0xC4 && Mockingboard::enabled && !internalRom) ? nullptr : sink;
    }
}

void slotsChanged() { pageSlots(); }

static void pageAll();

// //c à ROM de 32 Ko : $C028 échange les deux moitiés, partout de $C100 à $FFFF
void romBankChanged() {
    rom = romBase + (IIc::romBank() ? 0x4000 : 0);
    pageAll();
}

static void pageAll() {
    pageRange(0x02, 0xBF);
    pageZpAndLc();
    rdPage[0xC0] = wrPage[0xC0] = nullptr;
    rdPage[0xCF] = wrPage[0xCF] = nullptr;
    pageSlots();
    // //c : $CF00-$CFFF est de la ROM comme le reste, sans le rôle de $CFFF
    if (iic) rdPage[0xCF] = (uint8_t*)rom + 0x0F00;
    // //c Plus, moitié haute de la ROM : le circuit MIG prend $CC00 et $CE00
    if (iic && IIc::migVisible())
        rdPage[0xCC] = wrPage[0xCC] = rdPage[0xCE] = wrPage[0xCE] = nullptr;
}

// Pages vidéo seules : PAGE2 et HIRES ne déplacent qu'elles
static void pageVideo() {
    pageRange(0x04, 0x07);
    pageRange(0x20, 0x3F);
}

// ---------------------------------------------------------------------------
// Soft switches
// ---------------------------------------------------------------------------

// $C080-$C08F : carte langage. Deux lectures de suite d'une adresse impaire
// autorisent l'écriture.
static void lcSwitch(uint8_t reg, bool isWrite) {
    if (reg & 0x08) sw &= ~SW_LCBANK2; else sw |= SW_LCBANK2;
    if (((reg & 0x03) == 0x00) || ((reg & 0x03) == 0x03)) sw |= SW_LCRAM; else sw &= ~SW_LCRAM;
    if (reg & 0x01) {
        if (isWrite) {
            sw &= ~SW_LCPREWRITE;
        } else {
            if (sw & SW_LCPREWRITE) sw |= SW_LCWRITE;
            sw |= SW_LCPREWRITE;
        }
    } else {
        sw &= ~(SW_LCPREWRITE | SW_LCWRITE);
    }
    pageZpAndLc();
}

// $C050-$C05F : affichage et annonciateurs
static void displaySwitch(uint8_t reg) {
    uint32_t old = sw;
    switch (reg & 0x0F) {
        case 0x0: sw &= ~SW_TEXT; break;
        case 0x1: sw |= SW_TEXT; break;
        case 0x2: sw &= ~SW_MIXED; break;
        case 0x3: sw |= SW_MIXED; break;
        case 0x4: sw &= ~SW_PAGE2; break;
        case 0x5: sw |= SW_PAGE2; break;
        case 0x6: sw &= ~SW_HIRES; break;
        case 0x7: sw |= SW_HIRES; break;
        case 0xE: if (iie) sw |= SW_DHIRES; break;
        case 0xF: if (iie) sw &= ~SW_DHIRES; break;
        default: break;
    }
    if (((old ^ sw) & (SW_PAGE2 | SW_HIRES)) && (sw & SW_80STORE)) pageVideo();
}

// $C000-$C00F en écriture (//e) : mémoire et affichage
static void memorySwitch(uint8_t reg) {
    static const uint32_t flag[8] = { SW_80STORE, SW_RAMRD, SW_RAMWRT, SW_INTCXROM,
                                      SW_ALTZP, SW_SLOTC3ROM, SW_80COL, SW_ALTCHAR };
    uint32_t f = flag[(reg >> 1) & 7];
    // //c : pas de slots, donc pas de choix entre ROM interne et ROM de carte
    if (iic && (f == SW_INTCXROM || f == SW_SLOTC3ROM)) return;
    uint32_t old = sw;
    if (reg & 1) sw |= f; else sw &= ~f;
    if (old == sw) return;
    if (f == SW_ALTZP) pageZpAndLc();
    else if (f == SW_RAMRD || f == SW_RAMWRT) pageRange(0x02, 0xBF);
    else if (f == SW_80STORE) pageVideo();
    else if (f == SW_INTCXROM || f == SW_SLOTC3ROM) pageSlots();
}

static inline void speakerToggle() {
    spkLevel = !spkLevel;
    if (spkCount < SPK_MAX_EVENTS) {
        uint32_t t = cycles - frameStartCycles;
        spkEvents[spkCount++] = t > 0xFFFF ? 0xFFFF : (uint16_t)t;
    }
}

// $C100-$CFFF : ROM des cartes et ROM interne du //e
static uint8_t slotRead(uint16_t addr) {
    uint8_t page = addr >> 8;
    if (iic) {
        int v = IIc::migRead(addr);
        return v >= 0 ? (uint8_t)v : rom[addr - 0xC000];
    }
    // Les pages des cartes présentes ($C6, $C7) sont lues directement (pageSlots)
    if (!iie) return (page == 0xC4 && Mockingboard::enabled) ? Mockingboard::read(addr) : videoFloatingBus();

    if (page >= 0xC8) {
        uint8_t v = (sw & (SW_INTCXROM | SW_INTC8ROM)) ? rom[addr - 0xC000] : videoFloatingBus();
        if (addr == 0xCFFF && (sw & SW_INTC8ROM)) {
            sw &= ~SW_INTC8ROM;
            pageSlots();
        }
        return v;
    }
    if (sw & SW_INTCXROM) return rom[addr - 0xC000];
    if (page == 0xC3) {
        // Sans carte dans le slot 3, le firmware 80 colonnes interne répond, et
        // prend $C800-$CFFF. Sans mémoire auxiliaire il n'y a pas de carte 80 colonnes.
        if (!(sw & SW_SLOTC3ROM) && auxRam) {
            if (!(sw & SW_INTC8ROM)) {
                sw |= SW_INTC8ROM;
                pageSlots();
            }
            return rom[addr - 0xC000];
        }
        return videoFloatingBus();
    }
    if (page == 0xC4 && Mockingboard::enabled) return Mockingboard::read(addr);
    return videoFloatingBus();
}

uint8_t ioRead(uint16_t addr) {
    if (addr >= 0xC100) return slotRead(addr);
    uint8_t reg = addr & 0xFF;
    if (iic) {
        // Ce que le //c a en propre : souris, retour vertical, ports série
        int v = IIc::read(reg, keyLatch & 0x7F);
        if (v >= 0) return (uint8_t)v;
    }
    switch (reg >> 4) {
        case 0x0:
            return keyLatch;
        case 0x1:
            if (!iie) {
                keyLatch &= 0x7F;
                return videoFloatingBus();
            }
            switch (reg) {
                case 0x10: {
                    uint8_t v = (anyKeyDown ? 0x80 : 0) | (keyLatch & 0x7F);
                    keyLatch &= 0x7F;
                    return v;
                }
                case 0x11: return ((sw & SW_LCBANK2) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x12: return ((sw & SW_LCRAM) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x13: return ((sw & SW_RAMRD) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x14: return ((sw & SW_RAMWRT) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x15: return ((sw & SW_INTCXROM) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x16: return ((sw & SW_ALTZP) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x17: return ((sw & SW_SLOTC3ROM) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x18: return ((sw & SW_80STORE) ? 0x80 : 0) | (keyLatch & 0x7F);
                // Bit 7 à 1 hors du retour vertical
                case 0x19: return (scanline < A2_VISIBLE_LINES ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x1A: return ((sw & SW_TEXT) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x1B: return ((sw & SW_MIXED) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x1C: return ((sw & SW_PAGE2) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x1D: return ((sw & SW_HIRES) ? 0x80 : 0) | (keyLatch & 0x7F);
                case 0x1E: return ((sw & SW_ALTCHAR) ? 0x80 : 0) | (keyLatch & 0x7F);
                default:   return ((sw & SW_80COL) ? 0x80 : 0) | (keyLatch & 0x7F);
            }
        case 0x3:
            speakerToggle();
            return videoFloatingBus();
        case 0x5:
            displaySwitch(reg);
            return videoFloatingBus();
        case 0x6:
            switch (reg & 0x07) {
                case 1: case 2: case 3:
                    return (buttons[(reg & 7) - 1] ? 0x80 : 0) | (videoFloatingBus() & 0x7F);
                case 4: case 5: case 6: case 7: {
                    // Le monostable de la manette reste haut 11 cycles par unité
                    uint32_t elapsed = cycles - paddleStart;
                    bool high = elapsed < (uint32_t)paddles[reg & 3] * 11 + 8;
                    return (high ? 0x80 : 0) | (videoFloatingBus() & 0x7F);
                }
                default: return videoFloatingBus() & 0x7F;   // entrée cassette
            }
        case 0x7:
            paddleStart = cycles;
            if (iie && reg == 0x7F) return ((sw & SW_DHIRES) ? 0x80 : 0) | (videoFloatingBus() & 0x7F);
            return videoFloatingBus();
        case 0x8:
            lcSwitch(reg, false);
            return videoFloatingBus();
        case 0xA:
            return (Mouse::enabled && !iic) ? Mouse::read(reg) : videoFloatingBus();
        case 0xE:
            return Disk::access(reg & 0x0F, false, 0);
        case 0xF:
            return iic ? videoFloatingBus() : Hdd::ioRead(reg & 0x0F);
        default:
            return videoFloatingBus();
    }
}

void ioWrite(uint16_t addr, uint8_t value) {
    if (addr >= 0xC100) {
        if (iic) { IIc::migWrite(addr, value); return; }
        if ((addr >> 8) == 0xC4 && Mockingboard::enabled && !iic) Mockingboard::write(addr, value);
        if (iie && addr == 0xCFFF && (sw & SW_INTC8ROM)) {
            sw &= ~SW_INTC8ROM;
            pageSlots();
        }
        return;
    }
    uint8_t reg = addr & 0xFF;
    if (iic && IIc::write(reg, value)) return;
    switch (reg >> 4) {
        case 0x0: if (iie) memorySwitch(reg); break;
        case 0x1: keyLatch &= 0x7F; break;
        case 0x3: speakerToggle(); break;
        case 0x5: displaySwitch(reg); break;
        case 0x7: paddleStart = cycles; break;
        case 0x8: lcSwitch(reg, true); break;
        case 0xA: if (Mouse::enabled && !iic) Mouse::write(reg, value); break;
        case 0xE: Disk::access(reg & 0x0F, true, value); break;
        case 0xF: if (!iic) Hdd::ioWrite(reg & 0x0F, value); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Clavier et manettes
// ---------------------------------------------------------------------------

void keyDown(uint8_t ascii) {
    // ][ et ][+ : clavier sans minuscules
    if (!iie && ascii >= 'a' && ascii <= 'z') ascii -= 0x20;
    keyLatch = ascii | 0x80;
    anyKeyDown = true;
}

void keyUp() { anyKeyDown = false; }

int unescape(const char* text, char* out, int maxLen) {
    int n = 0;
    for (size_t i = 0; text[i] && n < maxLen; i++) {
        char c = text[i];
        if (c == '\\' && text[i + 1]) {
            char e = text[++i];
            if (e == 'r') c = 0x0D;
            else if (e == 'e') c = 0x1B;
            else if (e == 'x' && text[i + 1] && text[i + 2]) {
                c = 0;
                for (int k = 1; k <= 2; k++) {
                    char h = text[i + k];
                    c = (char)(c * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0'));
                }
                i += 2;
            } else c = e;
        }
        out[n++] = c;
    }
    return n;
}

bool keyWaiting() { return (keyLatch & 0x80) != 0; }

void setButton(int n, bool down) { if (n >= 0 && n < 3) buttons[n] = down; }

void setPaddle(int n, uint8_t value) { if (n >= 0 && n < 4) paddles[n] = value; }

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

void init(uint8_t* main64k, uint8_t* aux64k) {
    mainRam = main64k;
    auxRam = aux64k;
    Disk::init();
    Disk35::init();
    Hdd::init();
    setModel(curModel);
}

void setModel(Model m) {
    curModel = m;
    iic = (m >= MODEL_IIC);
    iie = (m == MODEL_IIE || m == MODEL_IIE_ENH || iic);
    cpu.cmos = (m == MODEL_IIE_ENH || iic);
    IIc::setRomVersion(m >= MODEL_IIC0, m >= MODEL_IIC3, m == MODEL_IICPLUS);
    Disk::setIwm(iic);
    Disk::setPlus(m == MODEL_IICPLUS);
    SmartPort::setEnabled(m >= MODEL_IIC0);
    SmartPort::setInternal35(m == MODEL_IICPLUS);
    Disk35::setPresent(m == MODEL_IICPLUS);
    switch (m) {
        case MODEL_II:      rom = gb_rom_apple2; break;
        case MODEL_IIPLUS:  rom = gb_rom_apple2plus; break;
        case MODEL_IIE:     rom = gb_rom_apple2e; break;
        case MODEL_IIC:     rom = gb_rom_apple2c; break;
        case MODEL_IIC0:    rom = gb_rom_apple2c0; break;
        case MODEL_IIC3:    rom = gb_rom_apple2c3; break;
        case MODEL_IIC4:    rom = gb_rom_apple2c4; break;
        case MODEL_IICPLUS: rom = gb_rom_apple2cp; break;
        default:            rom = gb_rom_apple2e_enh; break;
    }
    romBase = rom;
    videoSetModel();
    powerOn();
}

void reset() {
    // Le //e remet ses bascules mémoire à zéro ; la carte langage revient en
    // lecture ROM, écriture autorisée, banque 2
    sw = SW_TEXT | SW_LCBANK2 | SW_LCWRITE;
    IIc::reset();
    rom = romBase;
    anyKeyDown = false;
    pageAll();
    Disk::reset();
    Mockingboard::reset();
    Mouse::reset();
    cpuReset();
}

void powerOn() {
    if (!mainRam) return;
    powerOnCount++;
    // Motif de la RAM à la mise sous tension : FF FF 00 00
    for (int i = 0; i < 0x10000; i += 4) {
        mainRam[i] = mainRam[i + 1] = 0xFF;
        mainRam[i + 2] = mainRam[i + 3] = 0x00;
    }
    if (auxRam) memcpy(auxRam, mainRam, 0x10000);
    keyLatch = 0;
    spkLevel = spkFrameLevel = false;
    spkCount = 0;
    cpu.a = cpu.x = cpu.y = 0;
    cpu.p = 0x24;
    reset();
}

void runFrame() {
    frameStartCycles = cycles;
    Mockingboard::frameBegin();
    spkFrameLevel = spkLevel;
    spkCount = 0;
    alignas(4) static uint8_t lineBuf[A2_LINE_PIXELS + 4];
    for (scanline = 0; scanline < A2_LINES_PER_FRAME; scanline++) {
        lineStartCycles = cycles;
        if (iic) IIc::scanlineTick();
        if (scanline == A2_VISIBLE_LINES) {
            if (iic) IIc::vbl();
            else Mouse::vbl();
        }
        cpuRun(A2_CYCLES_PER_LINE);
        if (scanline < A2_VISIBLE_LINES && renderEnabled && renderLine(scanline, lineBuf))
            A2_platformLine(scanline, lineBuf);
    }
    frameCount++;
    Disk::frameTick();
    Hdd::frameTick();
}

// ---------------------------------------------------------------------------
// Sauvegarde d'état
// ---------------------------------------------------------------------------

static const char stateMagic[4] = { 'A', '2', 'S', '5' };

static void machineState(StateIO& io) {
    cpuState(io);
    io.value(sw); io.value(keyLatch); io.value(anyKeyDown); io.value(paddleStart);
    io.value(spkLevel); io.value(frameCount);
    io.bytes(mainRam, 0x10000);
    if (auxRam) io.bytes(auxRam, 0x10000);
    Disk::state(io);
    Disk35::state(io);
    SmartPort::state(io);
    Mockingboard::state(io);
    Mouse::state(io);
    IIc::state(io);
}

bool saveState(bool (*write)(void* ctx, void* data, uint32_t len), void* ctx) {
    StateIO io = { true, true, write, ctx };
    char magic[4];
    memcpy(magic, stateMagic, 4);
    uint8_t m = curModel, aux = auxRam ? 1 : 0;
    io.bytes(magic, 4); io.value(m); io.value(aux);
    machineState(io);
    return io.ok;
}

bool loadState(bool (*read)(void* ctx, void* data, uint32_t len), void* ctx) {
    StateIO io = { false, true, read, ctx };
    char magic[4];
    uint8_t m = 0, aux = 0;
    io.bytes(magic, 4); io.value(m); io.value(aux);
    // Une sauvegarde faite avec ou sans mémoire auxiliaire ne se reprend que de même
    if (!io.ok || memcmp(magic, stateMagic, 4) != 0 || m >= MODEL_COUNT || aux != (auxRam ? 1 : 0)) return false;
    // Reprendre un état n'est pas une mise sous tension
    uint32_t powerOns = powerOnCount;
    if (m != curModel) setModel((Model)m);
    machineState(io);
    if (!io.ok) {
        // Fichier tronqué : la mémoire est à moitié remplacée
        powerOn();
        return false;
    }
    powerOnCount = powerOns;
    rom = romBase + (iic && IIc::romBank() ? 0x4000 : 0);
    pageAll();
    videoInvalidate();
    spkFrameLevel = spkLevel;
    spkCount = 0;
    return true;
}

// Position du balayage dans la ligne en cours, en cycles (0 à 64)
int videoLineCycle() {
    uint32_t c = cycles - lineStartCycles;
    return c > 64 ? 64 : (int)c;
}

// ---------------------------------------------------------------------------
// Haut-parleur
// ---------------------------------------------------------------------------

// Chaque échantillon vaut la part du temps où la membrane est en position
// haute pendant sa durée ; un filtre passe-haut retire ensuite la composante
// continue (le haut-parleur de l'Apple n'a que deux positions).
void renderAudio(uint8_t* out, int count, int volume) {
    static int32_t lowPass = 0;
    const uint32_t step = ((uint32_t)A2_CYCLES_PER_FRAME << 12) / count;   // cycles par échantillon, 20.12
    bool level = spkFrameLevel;
    int ev = 0;
    uint32_t pos = 0;
    // Mockingboard : la somme de ses six voies, par échantillon
    static int32_t ayMix[1024];
    if (count > 1024) count = 1024;
    memset(ayMix, 0, count * sizeof(int32_t));
    bool music = Mockingboard::render(ayMix, count);
    for (int i = 0; i < count; i++) {
        uint32_t end = pos + step;
        uint32_t high = 0, t = pos;
        while (ev < spkCount && ((uint32_t)spkEvents[ev] << 12) < end) {
            uint32_t at = (uint32_t)spkEvents[ev] << 12;
            if (at > t) {
                if (level) high += at - t;
                t = at;
            }
            level = !level;
            ev++;
        }
        if (level) high += end - t;
        pos = end;

        // high <= step < 2^18 : le produit tient sur 32 bits
        int32_t x = (int32_t)((high << 12) / step) - 2048;   // -2048 à +2048
        // Six voies de 0 à 255 : une musique à trois voix a le niveau du haut-parleur
        if (music) x += ayMix[i] * 6;
        lowPass += (x - lowPass) >> 8;
        int32_t v = 128 + (((x - lowPass) * volume) >> 8);
        out[i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
    }
}

}
