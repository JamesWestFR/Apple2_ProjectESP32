/*

Apple2_ProjectESP32, an Apple II emulator for Espressif ESP32 SoC

Cœur d'émulation portable : 6502/65C02, mémoire et soft switches, vidéo,
haut-parleur, Disk II. Ce dossier ne dépend ni de l'ESP32 ni d'ESP-IDF : il se
compile aussi sur PC (dossier host/) pour les tests.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef A2_h
#define A2_h

#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define A2_FAST IRAM_ATTR
#else
#define A2_FAST
#endif

namespace A2 {

enum Model : uint8_t {
    MODEL_II = 0,     // Apple ][ : Integer BASIC, moniteur d'origine
    MODEL_IIPLUS,     // Apple ][+ : Applesoft, Autostart, carte langage 16 Ko
    MODEL_IIE,        // Apple //e : 6502, 128 Ko avec la carte 80 colonnes étendue
    MODEL_IIE_ENH,    // Apple //e Enhanced : 65C02, MouseText
    MODEL_COUNT
};

const char* modelName(Model m);

// --- Temps -------------------------------------------------------------------

#define A2_CYCLES_PER_LINE 65
#define A2_LINES_PER_FRAME 262
#define A2_CYCLES_PER_FRAME (A2_CYCLES_PER_LINE * A2_LINES_PER_FRAME)   // 17030, soit 59,92 images/s
#define A2_VISIBLE_LINES 192
#define A2_LINE_PIXELS 560

extern uint32_t cycles;        // cycles 6502 écoulés (boucle sur 32 bits : ne comparer que des écarts)
extern uint32_t frameCount;
extern int scanline;           // ligne en cours, 0 à 261

// --- 6502 / 65C02 ------------------------------------------------------------

struct CpuState {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    bool cmos;                 // 65C02 (//e Enhanced)
};
extern CpuState cpu;

void cpuReset();

// Interruptions : ligne IRQ (tenue par les 6522 de la Mockingboard), et instant
// du prochain événement de compteur, que le 6502 compare au temps courant
extern bool irqLine;
extern bool timerArmed;
extern uint32_t timerDue;
void timerEvent();
void cpuRun(int32_t budget);   // exécute au moins `budget` cycles ; le dépassement est reporté

// --- Mémoire -----------------------------------------------------------------

// Une entrée par page de 256 octets. nullptr : page d'entrées/sorties ($C000-$CFFF),
// l'accès passe par ioRead / ioWrite.
extern uint8_t* rdPage[256];
extern uint8_t* wrPage[256];

uint8_t ioRead(uint16_t addr);
void ioWrite(uint16_t addr, uint8_t value);

static inline uint8_t memRead(uint16_t addr) {
    uint8_t* p = rdPage[addr >> 8];
    return p ? p[addr & 0xFF] : ioRead(addr);
}

static inline void memWrite(uint16_t addr, uint8_t value) {
    uint8_t* p = wrPage[addr >> 8];
    if (p) p[addr & 0xFF] = value;
    else ioWrite(addr, value);
}

// Soft switches
enum : uint32_t {
    SW_80STORE    = 1 << 0,
    SW_RAMRD      = 1 << 1,
    SW_RAMWRT     = 1 << 2,
    SW_INTCXROM   = 1 << 3,
    SW_ALTZP      = 1 << 4,
    SW_SLOTC3ROM  = 1 << 5,
    SW_80COL      = 1 << 6,
    SW_ALTCHAR    = 1 << 7,
    SW_TEXT       = 1 << 8,
    SW_MIXED      = 1 << 9,
    SW_PAGE2      = 1 << 10,
    SW_HIRES      = 1 << 11,
    SW_DHIRES     = 1 << 12,   // annonciateur 3 à l'état bas
    SW_LCBANK2    = 1 << 13,
    SW_LCRAM      = 1 << 14,   // $D000-$FFFF lus en RAM
    SW_LCWRITE    = 1 << 15,
    SW_LCPREWRITE = 1 << 16,
    SW_INTC8ROM   = 1 << 17,   // ROM interne en $C800-$CFFF (interne au //e)
};
extern uint32_t sw;

// --- Machine -----------------------------------------------------------------

// `main64k` et `aux64k` : deux blocs de 65536 octets fournis par la plateforme.
// aux64k peut être nul : le //e n'a alors que 64 Ko, sans 80 colonnes.
void init(uint8_t* main64k, uint8_t* aux64k);
void setModel(Model m);
Model model();
bool isIIe();
void powerOn();            // démarrage à froid : RAM effacée
void reset();              // Ctrl-Reset
void runFrame();           // une image : 262 lignes de 65 cycles

// --- Clavier, manettes -------------------------------------------------------

void keyDown(uint8_t ascii);
void keyUp();
bool keyWaiting();                       // la touche précédente n'a pas encore été lue
// Texte à taper, tel que l'écrivent les outils de test : \r (Retour), \e (Échap),
// \xNN (code hexadécimal). Rend le nombre de caractères écrits dans `out`.
int unescape(const char* text, char* out, int maxLen);
void setButton(int n, bool down);        // 0 : Pomme ouverte, 1 : Pomme pleine, 2 : bouton 2
void setPaddle(int n, uint8_t value);    // 0 à 255, 127 au centre

// --- Vidéo -------------------------------------------------------------------

// Une ligne de 560 points, un octet par point : le numéro de la couleur Apple
// (0 à 15), ou la valeur que setPixelMap lui a associée.
// Appelée pour chaque ligne visible (0 à 191), sauf si renderEnabled est faux.
extern bool renderEnabled;
// Valeur à écrire pour chacune des 16 couleurs (l'octet du framebuffer, par
// exemple) : le rendu produit alors directement le format de la plateforme
void setPixelMap(const uint8_t* map16);
extern bool monochrome;                  // rendu sans couleurs d'artefact (moniteur mono)
// Rend la ligne si ce qu'elle montre a changé depuis son dernier rendu ; rend
// faux, sans toucher à `out`, si elle est restée la même
bool renderLine(int y, uint8_t* out);
// À appeler quand la plateforme a écrit elle-même dans l'image (menu, texte
// par-dessus) ou changé ses couleurs : ces lignes seront redessinées
void videoInvalidate(int firstLine = 0, int lastLine = A2_VISIBLE_LINES - 1);
// Rend une ligne de texte 40 colonnes à partir d'un tampon de 40 codes écran
// (pour le menu, qui s'affiche avec les caractères de l'Apple)
void renderTextRow40(const uint8_t* chars, int glyphLine, uint8_t* out, uint8_t fg, uint8_t bg);
uint8_t videoFloatingBus();
// Une ligne de l'écran texte (page 1), en ASCII et sans ses espaces de fin,
// pour les outils de diagnostic. `out` : 41 caractères, 81 en 80 colonnes.
void textScreenLine(int row, bool col80, char* out);

// Couleurs Apple en RGB 8 bits (ordre des couleurs basse résolution)
extern const uint8_t paletteRGB[16][3];

// --- Haut-parleur ------------------------------------------------------------

// À appeler après runFrame : produit `count` échantillons non signés 8 bits pour
// l'image qui vient d'être émulée. `volume` : 0 à 8.
void renderAudio(uint8_t* out, int count, int volume);

// --- Disk II (slot 6) --------------------------------------------------------

namespace Disk {

// FMT_HDD : image trop grande pour une disquette 5,25 pouces, à monter comme disque dur (Hdd)
enum Format : uint8_t { FMT_NONE = 0, FMT_DOS, FMT_PRODOS, FMT_NIB, FMT_HDD };

#define A2_NIB_TRACK_SIZE 6656

void init();
// Reconnaît une image de disquette d'après son extension (en minuscules), sa
// taille et son début (`head`, au moins 2 Ko si le fichier les a). Rend nullptr
// si l'image est utilisable, sinon la raison du refus. Pour FMT_HDD, `tracks`
// reçoit le nombre de blocs de 512 octets.
const char* identify(const char* ext, uint32_t fileSize, const uint8_t* head, uint32_t headLen,
                     Format* fmt, int* tracks, uint32_t* dataOffset);
void insert(int drive, Format fmt, int tracks, uint32_t dataOffset, bool writeProtected);
void eject(int drive);
bool spinning();
bool busy();               // le lecteur tourne et contient une disquette
void reset();              // signal RESET de l'Apple
int quarterTrack(int drive);   // position de la tête, en quarts de piste
void flush();              // écrit la piste modifiée dans l'image
void frameTick();
uint8_t access(uint8_t reg, bool isWrite, uint8_t value);

extern uint8_t activity[2];     // images restantes d'affichage du voyant
extern uint32_t statReads, statWrites;

}

// --- Carte son Mockingboard (slot 4) -----------------------------------------

namespace Mockingboard {

extern bool enabled;
void setEnabled(bool on);   // présence de la carte dans le slot 4
uint8_t read(uint16_t addr);
void write(uint16_t addr, uint8_t value);
void reset();
void frameBegin();
bool render(int32_t* mix, int count);

}

// --- Disque dur ProDOS (slot 7) ----------------------------------------------

namespace Hdd {

void init();
void insert(uint32_t blocks, bool writeProtected);
void eject();
bool inserted();
uint32_t blockCount();
void frameTick();
uint8_t ioRead(uint8_t reg);
void ioWrite(uint8_t reg, uint8_t value);

extern uint8_t activity;
extern uint32_t statReads, statWrites;

}

}

// --- À fournir par la plateforme --------------------------------------------

// Ligne d'image prête : 560 octets (voir setPixelMap), dans un tampon aligné sur 4 octets
void A2_platformLine(int y, const uint8_t* pixels);
// Accès à l'image de disquette insérée dans `drive` (0 ou 1)
bool A2_platformDiskRead(int drive, uint32_t offset, uint8_t* buf, uint32_t len);
bool A2_platformDiskWrite(int drive, uint32_t offset, const uint8_t* buf, uint32_t len);

// Blocs de 512 octets de l'image de disque dur
bool A2_platformHddRead(uint32_t block, uint8_t* buf);
bool A2_platformHddWrite(uint32_t block, const uint8_t* buf);

#endif // A2_h
