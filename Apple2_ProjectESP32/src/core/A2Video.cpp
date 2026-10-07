/*

Apple2_ProjectESP32 — Rendu vidéo, ligne par ligne.

Chaque ligne est rendue sur 560 points (la largeur des modes 80 colonnes et
double haute résolution), un index de couleur Apple par point. Les modes à 280
points occupent deux points chacun, ce qui laisse la place du décalage d'un
demi-point des octets haute résolution dont le bit 7 est à 1.

La ligne est rendue juste après les 65 cycles de 6502 qui lui correspondent :
un programme qui change de mode en cours d'image (texte sous un graphique,
changement de page) est rendu à la ligne près.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"
#include "roms_apple2.h"

namespace A2 {

bool monochrome = false;

// Couleurs Apple, dans l'ordre de la basse résolution
const uint8_t paletteRGB[16][3] = {
    { 0x00, 0x00, 0x00 },   //  0 noir
    { 0xDD, 0x00, 0x33 },   //  1 magenta
    { 0x00, 0x00, 0x99 },   //  2 bleu foncé
    { 0xDD, 0x22, 0xDD },   //  3 violet
    { 0x00, 0x77, 0x22 },   //  4 vert foncé
    { 0x55, 0x55, 0x55 },   //  5 gris 1
    { 0x22, 0x22, 0xFF },   //  6 bleu moyen
    { 0x66, 0xAA, 0xFF },   //  7 bleu clair
    { 0x88, 0x55, 0x00 },   //  8 marron
    { 0xFF, 0x66, 0x00 },   //  9 orange
    { 0xAA, 0xAA, 0xAA },   // 10 gris 2
    { 0xFF, 0x99, 0x88 },   // 11 rose
    { 0x11, 0xDD, 0x00 },   // 12 vert
    { 0xFF, 0xFF, 0x00 },   // 13 jaune
    { 0x44, 0xFF, 0x99 },   // 14 turquoise
    { 0xFF, 0xFF, 0xFF },   // 15 blanc
};

enum : uint8_t { BLACK = 0, VIOLET = 3, BLUE = 6, ORANGE = 9, GREEN = 12, WHITE = 15 };

// Le rendu n'écrit pas des numéros de couleur mais directement la valeur que
// la plateforme veut pour chacune (l'octet VGA sur l'ESP32) : il n'y a pas de
// seconde passe de conversion. Les tables qui suivent sont recalculées par
// setPixelMap ; sans appel, la valeur d'une couleur est son numéro.
static uint8_t pix[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static uint16_t pixBlack2 = 0, pixWhite2 = 0x0F0F;      // deux points de la même couleur
static uint32_t dhiresWord[16];                         // quatre points de la couleur d'un groupe de 4 bits
static uint8_t hiresPix[2][2][8];

// Haute résolution. Pour chaque point, selon ses deux voisins :
//   allumé, un voisin allumé     -> blanc
//   allumé, isolé                -> couleur de sa colonne (paire ou impaire)
//                                   dans la palette choisie par le bit 7 de l'octet
//   éteint entre deux allumés    -> la couleur de ses voisins (aplat de couleur)
//   sinon                        -> noir
// Index : [bit 7][colonne impaire][voisin gauche | point << 1 | voisin droit << 2]
static const uint8_t hiresColor[2][2][8] = {
    { { BLACK, BLACK, VIOLET, WHITE, BLACK, GREEN, WHITE, WHITE },
      { BLACK, BLACK, GREEN, WHITE, BLACK, VIOLET, WHITE, WHITE } },
    { { BLACK, BLACK, BLUE, WHITE, BLACK, ORANGE, WHITE, WHITE },
      { BLACK, BLACK, ORANGE, WHITE, BLACK, BLUE, WHITE, WHITE } },
};

static void buildPixelTables() {
    pixBlack2 = (uint16_t)(pix[BLACK] * 0x0101);
    pixWhite2 = (uint16_t)(pix[WHITE] * 0x0101);
    for (int n = 0; n < 16; n++) {
        // Double haute résolution : la couleur est le groupe de 4 bits, décalé d'un bit
        uint8_t color = (uint8_t)(((n << 1) | (n >> 3)) & 0x0F);
        dhiresWord[n] = pix[color] * 0x01010101u;
    }
    for (int hi = 0; hi < 2; hi++)
        for (int odd = 0; odd < 2; odd++)
            for (int w = 0; w < 8; w++) hiresPix[hi][odd][w] = pix[hiresColor[hi][odd][w]];
}

void setPixelMap(const uint8_t* map) {
    for (int i = 0; i < 16; i++) pix[i] = map[i];
    buildPixelTables();
    videoInvalidate();
}

static const uint8_t* charRom = gb_rom_video2e_enh;

void videoSetModel() {
    charRom = (model() == MODEL_IIE_ENH) ? gb_rom_video2e_enh : gb_rom_video2e;
    buildPixelTables();
    videoInvalidate();
}

static inline int textAddress(int row) {
    return 0x400 + ((row & 7) << 7) + ((row >> 3) * 40);
}

static inline int hiresAddress(int y) {
    return 0x2000 + ((y & 7) << 10) + (((y >> 3) & 7) << 7) + ((y >> 6) * 40);
}

// Code écran -> glyphe de la ROM de caractères du //e.
// ROM : $00-$3F inverse, $40-$7F MouseText ou inverse, $80-$FF normal.
static inline uint8_t glyphOf(uint8_t c, bool flashOn, bool iie, bool altChar) {
    if (iie) {
        // Jeu principal : $40-$7F clignote entre l'inverse et le normal
        if (!altChar && (c & 0xC0) == 0x40) return flashOn ? (c & 0x3F) : ((c & 0x3F) | 0x80);
        return c;
    }
    // ][ et ][+ : 64 caractères, majuscules seules
    uint8_t sel = c & 0x3F;
    if (c >= 0x80) return sel | 0x80;
    if (c >= 0x40) return flashOn ? sel : (sel | 0x80);
    return sel;
}

// Un caractère : 7 points, bit 0 à gauche, point allumé quand le bit est à 0
#define GLYPH_BITS(glyph, line) ((uint8_t)(~charRom[((glyph) << 3) + (line)]) & 0x7F)

// Les tampons de ligne sont alignés sur 4 octets : les points doublés
// s'écrivent par mots de 16 bits
A2_FAST static void renderText40(const uint8_t* src, int line, uint8_t* out, bool flashOn) {
    const bool iie = isIIe(), altChar = (sw & SW_ALTCHAR) != 0;
    const uint16_t on = pixWhite2, off = pixBlack2;
    uint16_t* dst = (uint16_t*)out;
    for (int col = 0; col < 40; col++) {
        uint32_t bits = GLYPH_BITS(glyphOf(src[col], flashOn, iie, altChar), line);
        dst[0] = (bits & 0x01) ? on : off;
        dst[1] = (bits & 0x02) ? on : off;
        dst[2] = (bits & 0x04) ? on : off;
        dst[3] = (bits & 0x08) ? on : off;
        dst[4] = (bits & 0x10) ? on : off;
        dst[5] = (bits & 0x20) ? on : off;
        dst[6] = (bits & 0x40) ? on : off;
        dst += 7;
    }
}

A2_FAST static void renderText80(const uint8_t* aux, const uint8_t* mainp, int line, uint8_t* out, bool flashOn) {
    const bool iie = isIIe(), altChar = (sw & SW_ALTCHAR) != 0;
    const uint8_t on = pix[WHITE], off = pix[BLACK];
    for (int col = 0; col < 40; col++) {
        uint32_t bits = GLYPH_BITS(glyphOf(aux[col], flashOn, iie, altChar), line)
                      | (GLYPH_BITS(glyphOf(mainp[col], flashOn, iie, altChar), line) << 7);
        for (int i = 0; i < 14; i++) {
            *out++ = (bits & 1) ? on : off;
            bits >>= 1;
        }
    }
}

void renderTextRow40(const uint8_t* chars, int glyphLine, uint8_t* out, uint8_t fg, uint8_t bg) {
    const uint8_t on = pix[fg & 15], off = pix[bg & 15];
    for (int col = 0; col < 40; col++) {
        uint8_t bits = GLYPH_BITS(chars[col], glyphLine);
        for (int i = 0; i < 7; i++) {
            out[0] = out[1] = (bits & 1) ? on : off;
            out += 2;
            bits >>= 1;
        }
    }
}

// Un bloc basse résolution sur un moniteur monochrome : le motif de 4 bits de
// sa couleur, répété
static inline uint8_t loresMono(uint8_t color, int x) {
    uint8_t pattern = (uint8_t)(((color >> 1) | (color << 3)) & 0x0F);
    return ((pattern >> (x & 3)) & 1) ? pix[WHITE] : pix[BLACK];
}

A2_FAST static void renderLores(const uint8_t* src, int line, uint8_t* out) {
    bool bottom = (line & 4) != 0;
    int x = 0;
    for (int col = 0; col < 40; col++) {
        uint8_t c = bottom ? (src[col] >> 4) : (src[col] & 0x0F);
        if (monochrome) {
            for (int i = 0; i < 14; i++, x++) *out++ = loresMono(c, x);
        } else {
            memset(out, pix[c], 14);
            out += 14;
        }
    }
}

// Double basse résolution : 80 blocs, les couleurs de la mémoire auxiliaire
// sont décalées d'un bit
A2_FAST static void renderDoubleLores(const uint8_t* aux, const uint8_t* mainp, int line, uint8_t* out) {
    bool bottom = (line & 4) != 0;
    int x = 0;
    for (int col = 0; col < 40; col++) {
        uint8_t a = bottom ? (aux[col] >> 4) : (aux[col] & 0x0F);
        uint8_t m = bottom ? (mainp[col] >> 4) : (mainp[col] & 0x0F);
        a = (uint8_t)(((a << 1) | (a >> 3)) & 0x0F);
        if (monochrome) {
            for (int i = 0; i < 7; i++, x++) *out++ = loresMono(a, x);
            for (int i = 0; i < 7; i++, x++) *out++ = loresMono(m, x);
        } else {
            memset(out, pix[a], 7);
            memset(out + 7, pix[m], 7);
            out += 14;
        }
    }
}

A2_FAST static void renderHires(const uint8_t* src, uint8_t* out) {
    const uint8_t black = pix[BLACK], white = pix[WHITE];
    const bool mono = monochrome;
    uint32_t prevBit = 0;
    uint8_t last = black;          // dernier point écrit
    uint8_t* px = out;
    int odd = 0;                   // parité de la colonne du premier point de l'octet
    for (int col = 0; col < 40; col++) {
        uint8_t b = src[col];
        uint32_t next = (col < 39) ? (src[col + 1] & 1) : 0;
        // bit 0 : dernier point de l'octet précédent ; bits 1 à 7 : les 7 points ; bit 8 : le suivant
        uint32_t bits = prevBit | ((uint32_t)(b & 0x7F) << 1) | (next << 8);
        int hi = b >> 7;
        // Octet retardé d'un demi-point : le point précédent se prolonge, et
        // le dernier point déborde d'un demi-point sur l'octet suivant
        uint8_t* p = px;
        if (hi) *p++ = last;
        if (mono) {
            for (int i = 0; i < 7; i++) {
                last = ((bits >> (i + 1)) & 1) ? white : black;
                p[0] = p[1] = last;
                p += 2;
            }
        } else {
            const uint8_t* even = hiresPix[hi][odd];
            const uint8_t* oddc = hiresPix[hi][odd ^ 1];
            uint8_t c;
            c = even[bits & 7];        p[0] = p[1] = c;
            c = oddc[(bits >> 1) & 7]; p[2] = p[3] = c;
            c = even[(bits >> 2) & 7]; p[4] = p[5] = c;
            c = oddc[(bits >> 3) & 7]; p[6] = p[7] = c;
            c = even[(bits >> 4) & 7]; p[8] = p[9] = c;
            c = oddc[(bits >> 5) & 7]; p[10] = p[11] = c;
            c = even[(bits >> 6) & 7]; p[12] = p[13] = c;
            last = c;
        }
        prevBit = (b >> 6) & 1;
        px += 14;
        odd ^= 1;                  // 7 points par octet : la parité alterne
    }
}

// Double haute résolution : 560 points, pris 7 par 7 dans la mémoire
// auxiliaire puis principale. En couleur, chaque groupe de 4 points donne une
// des 16 couleurs (140 points de couleur par ligne).
A2_FAST static void renderDoubleHires(const uint8_t* aux, const uint8_t* mainp, uint8_t* out) {
    const uint8_t black = pix[BLACK], white = pix[WHITE];
    uint32_t* dst = (uint32_t*)out;
    for (int col = 0; col < 40; col += 2) {
        uint32_t w = (uint32_t)(aux[col] & 0x7F) | ((uint32_t)(mainp[col] & 0x7F) << 7)
                   | ((uint32_t)(aux[col + 1] & 0x7F) << 14) | ((uint32_t)(mainp[col + 1] & 0x7F) << 21);
        if (monochrome) {
            for (int i = 0; i < 28; i++) {
                *out++ = (w & 1) ? white : black;
                w >>= 1;
            }
        } else {
            for (int k = 0; k < 7; k++) {
                *dst++ = dhiresWord[w & 0x0F];
                w >>= 4;
            }
        }
    }
}

// Signature de ce que chaque ligne affichait à son dernier rendu : le mode et
// les 40 ou 80 octets de mémoire vidéo qu'elle montre. Une ligne dont la
// signature n'a pas changé n'est pas redessinée : sur un écran fixe, le rendu
// ne coûte presque rien. L'adresse de la page n'entre pas dans la signature,
// seulement son contenu : un programme qui alterne deux pages ne fait
// redessiner que les lignes qui diffèrent.
static uint32_t lineSignature[A2_VISIBLE_LINES];
static bool lineValid[A2_VISIBLE_LINES];

void videoInvalidate(int first, int last) {
    if (first < 0) first = 0;
    if (last >= A2_VISIBLE_LINES) last = A2_VISIBLE_LINES - 1;
    for (int y = first; y <= last; y++) lineValid[y] = false;
}

// 40 octets, lus par mots : les lignes de la mémoire vidéo commencent toutes à
// une adresse multiple de 4
static inline uint32_t hash40(uint32_t h, const uint8_t* src) {
    const uint32_t* w = (const uint32_t*)src;
    for (int i = 0; i < 10; i++) h = (h ^ w[i]) * 0x9E3779B1u + (h >> 15);
    return h;
}

enum { KIND_TEXT40 = 1, KIND_TEXT80, KIND_LORES, KIND_DLORES, KIND_HIRES, KIND_DHIRES };

A2_FAST bool renderLine(int y, uint8_t* out) {
    const uint8_t* mainRam = ramMain();
    const uint8_t* auxRam = ramAux();
    bool col80 = isIIe() && auxRam && (sw & SW_80COL);
    bool page2 = (sw & SW_PAGE2) && !(sw & SW_80STORE);
    bool dbl = col80 && (sw & SW_DHIRES);
    bool flashOn = (frameCount & 0x10) != 0;

    // Ce que la ligne montre : le mode, et où sont ses octets
    int kind, addr;
    uint32_t mode = monochrome ? 0x100 : 0;
    if ((sw & SW_TEXT) || ((sw & SW_MIXED) && y >= 160)) {
        kind = col80 ? KIND_TEXT80 : KIND_TEXT40;
        addr = textAddress(y >> 3);
        mode |= (flashOn ? 0x200 : 0) | ((sw & SW_ALTCHAR) ? 0x400 : 0);
    } else if (sw & SW_HIRES) {
        kind = dbl ? KIND_DHIRES : KIND_HIRES;
        addr = hiresAddress(y);
    } else {
        kind = dbl ? KIND_DLORES : KIND_LORES;
        addr = textAddress(y >> 3);
    }
    bool twoSources = kind == KIND_TEXT80 || kind == KIND_DLORES || kind == KIND_DHIRES;
    if (!twoSources && page2) addr += (kind == KIND_HIRES) ? 0x2000 : 0x400;
    const uint8_t* src = mainRam + addr;
    const uint8_t* src2 = twoSources ? auxRam + addr : nullptr;

    uint32_t h = hash40(mode | kind, src);
    if (src2) h = hash40(h, src2);
    if (lineValid[y] && lineSignature[y] == h) return false;
    lineSignature[y] = h;
    lineValid[y] = true;

    switch (kind) {
        case KIND_TEXT40: renderText40(src, y & 7, out, flashOn); break;
        case KIND_TEXT80: renderText80(src2, src, y & 7, out, flashOn); break;
        case KIND_LORES:  renderLores(src, y & 7, out); break;
        case KIND_DLORES: renderDoubleLores(src2, src, y & 7, out); break;
        case KIND_HIRES:  renderHires(src, out); break;
        default:          renderDoubleHires(src2, src, out); break;
    }
    return true;
}

void textScreenLine(int row, bool col80, char* out) {
    const uint8_t* mainRam = ramMain();
    const uint8_t* auxRam = ramAux();
    int addr = textAddress(row), len = 0;
    for (int col = 0; col < 40; col++) {
        for (int half = (col80 && auxRam) ? 0 : 1; half < 2; half++) {
            // Inverse et clignotant sont rendus comme le caractère normal
            uint8_t c = (half ? mainRam : auxRam)[addr + col] & 0x7F;
            if (c < 0x20) c += 0x40;
            out[len++] = (c < 0x7F) ? (char)c : '.';
        }
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = 0;
}

// Bus flottant : sans périphérique pour répondre, une lecture rend l'octet que
// le circuit vidéo lit à cet instant. Certains programmes s'en servent pour se
// synchroniser sur le balayage.
uint8_t videoFloatingBus() {
    const uint8_t* mainRam = ramMain();
    if (!mainRam) return 0;
    // 25 cycles de retour horizontal, puis 40 octets visibles
    int col = videoLineCycle() - 25;
    if (col < 0) col = 0;
    if (col > 39) col = 39;
    int y = scanline < A2_VISIBLE_LINES ? scanline : scanline - A2_VISIBLE_LINES;
    bool page2 = (sw & SW_PAGE2) && !(sw & SW_80STORE);
    bool text = (sw & SW_TEXT) || ((sw & SW_MIXED) && y >= 160);
    if (!text && (sw & SW_HIRES)) return mainRam[hiresAddress(y) + (page2 ? 0x2000 : 0) + col];
    return mainRam[textAddress(y >> 3) + (page2 ? 0x400 : 0) + col];
}

}
