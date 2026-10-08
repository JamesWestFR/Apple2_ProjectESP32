/*

Apple2_ProjectESP32 — Sortie VGA (pilote ESP32Lib de bitluni, repris d'ESPectrum).

Le framebuffer fait 560 x 192 points, un octet par point (2 bits par couleur
et les deux bits de synchro). Il est envoyé en continu par DMA, chaque ligne
deux fois, dans un signal VGA 640x480 à 60 Hz. L'émulation n'a qu'à y écrire.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "soc/i2s_reg.h"
#include "Video.h"
#include "ESPConfig.h"
#include "hardpins.h"
#include "core/A2.h"
#include "ESP32Lib/VGA/VGA6Bit.h"

namespace Video {

static VGA6Bit vga;
volatile bool vsync = false;
bool ready = false;

static const int redPins[] = { RED_PINS_6B };
static const int grePins[] = { GRE_PINS_6B };
static const int bluPins[] = { BLU_PINS_6B };

// Couleur Apple (0 à 15) -> octet VGA, bits de synchro compris
static DRAM_ATTR uint8_t palette[16];

// Adresse du descripteur DMA de la dernière ligne d'image
static uint32_t lastLineDescriptor = 0;

static void* blackLine = nullptr;

// R2G2B2 : R sur les bits 0-1, G sur 2-3, B sur 4-5
static uint8_t vgaColor(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t c = ((r * 3 + 127) / 255) | (((g * 3 + 127) / 255) << 2) | (((b * 3 + 127) / 255) << 4);
    return (c & vga.RGBAXMask) | vga.SBits;
}

void setMonitor(uint8_t type) {
    for (int i = 0; i < 16; i++)
        palette[i] = vgaColor(A2::paletteRGB[i][0], A2::paletteRGB[i][1], A2::paletteRGB[i][2]);
    A2::monochrome = type != MONITOR_COLOR;
    A2::dhiresMono = Config::dhgrMono != 0;
    A2::hiresFringes = Config::fringes != 0;
    A2::setCharset(Config::charset);
    // En monochrome le rendu n'utilise que le noir et le blanc : le blanc prend
    // la teinte du phosphore
    if (type == MONITOR_GREEN) palette[15] = vgaColor(0x55, 0xFF, 0x55);
    else if (type == MONITOR_AMBER) palette[15] = vgaColor(0xFF, 0xAA, 0x00);
    // Le rendu écrit directement ces octets : pas de conversion ensuite
    A2::setPixelMap(palette);
}

// La liste DMA (VGA::allocateLineBuffers) : un descripteur par ligne de retour
// vertical, puis deux par ligne VGA visible (palier et synchro, puis image).
// Chaque ligne du framebuffer y figure deux fois ; les « scanlines » remplacent
// la seconde par une ligne noire.
void setScanlines(bool on) {
    if (!ready) return;
    const unsigned short* m = vidmodes[vga.mode];
    if (on && !blackLine) {
        blackLine = heap_caps_malloc(vga.xres, MALLOC_CAP_DMA);
        if (!blackLine) return;
        memset(blackLine, vga.SBits, vga.xres);
    }
    int first = m[vmodeproperties::vFront] + m[vmodeproperties::vSync] + m[vmodeproperties::vBack];
    for (int i = 0; i < m[vmodeproperties::vRes]; i++) {
        void* line = (on && (i & 1)) ? blackLine : (void*)vga.frameBuffer[i >> 1];
        vga.dmaBufferDescriptors[first + 2 * i + 1].setBuffer(line, vga.xres);
    }
}

// La VGA est initialisée depuis une tâche du cœur 1 pour que son interruption
// I2S y soit rattachée et ne perturbe pas l'émulation (cœur 0)
static void vgaTaskInit(void* unused) {
    vga.VGA6Bit_useinterrupt = true;
    vga.init(0, redPins, grePins, bluPins, HSYNC_PIN, VSYNC_PIN);
    const unsigned short* m = vidmodes[vga.mode];
    int count = m[vmodeproperties::vFront] + m[vmodeproperties::vSync] + m[vmodeproperties::vBack]
              + 2 * m[vmodeproperties::vRes];
    lastLineDescriptor = (uint32_t)&vga.dmaBufferDescriptors[count - 1];
    ready = true;
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void init() {
    xTaskCreatePinnedToCore(&vgaTaskInit, "videoTask", 2048, NULL, 5, NULL, 1);

    // Attend la fin de l'init VGA (3 s au plus)
    int64_t start = esp_timer_get_time();
    while (!ready && (esp_timer_get_time() - start) < 3000000) vTaskDelay(pdMS_TO_TICKS(10));
    if (!ready) printf("WARN: VGA init timeout (continuing without display)\n");

    setMonitor(Config::monitor);
    clear();
    setScanlines(Config::scanlines);
}

void clear() {
    if (!ready) return;
    for (int y = 0; y < vga.yres; y++) memset(vga.frameBuffer[y], vga.SBits, vga.xres);
    A2::videoInvalidate();
}

void drawTextRow(int row, const uint8_t* chars, uint8_t fg, uint8_t bg) {
    alignas(4) uint8_t line[A2_LINE_PIXELS];
    for (int i = 0; i < 8; i++) {
        A2::renderTextRow40(chars, i, line, fg, bg);
        A2_platformLine(row * 8 + i, line);
    }
    // Ces lignes ne montrent plus l'écran de l'Apple : à redessiner à la reprise
    A2::videoInvalidate(row * 8, row * 8 + 7);
}

void overlayText(int col, int row, const char* text, bool inverse) {
    if (!ready || row < 0 || row > 23) return;
    uint8_t chars[40];
    int len = 0;
    while (text[len] && col + len < 40) len++;
    if (col < 0 || len == 0) return;
    // Codes écran du jeu de caractères complet : $80 + ASCII
    memset(chars, 0xA0, sizeof(chars));
    for (int i = 0; i < len; i++) chars[col + i] = (uint8_t)text[i] | 0x80;
    uint8_t line[A2_LINE_PIXELS];
    for (int i = 0; i < 8; i++) {
        A2::renderTextRow40(chars, i, line, inverse ? 0 : 15, inverse ? 15 : 0);
        uint8_t* dst = vga.frameBuffer[row * 8 + i];
        // Dans le framebuffer, les octets vont par mots de 32 bits, moitiés échangées
        for (int x = col * 14; x < (col + len) * 14; x++) dst[x ^ 2] = line[x];
    }
    // L'image de l'Apple sera redessinée dessous quand le texte disparaîtra
    A2::videoInvalidate(row * 8, row * 8 + 7);
}

void printSummary() {
    if (!ready) { printf("A2 video: not ready\n"); return; }
    printf("A2 video: %dx%d, lit pixels per text row:", vga.xres, vga.yres);
    for (int row = 0; row < 24; row++) {
        int lit = 0;
        for (int y = row * 8; y < row * 8 + 8; y++) {
            const uint8_t* src = vga.frameBuffer[y];
            for (int x = 0; x < vga.xres; x++) lit += (src[x] & 0x3F) != 0;
        }
        printf(" %d", lit);
    }
    printf("\n");
}

// Diagnostic : le framebuffer en hexadécimal sur le port série, une ligne par
// ligne d'image, deux chiffres par point (couleur VGA sur 6 bits, dans l'ordre
// de l'écran). scripts/a2console.py en fait une image PNG.
void dumpFramebuffer() {
    if (!ready) { printf("A2 video: not ready\n"); return; }
    static const char hex[] = "0123456789abcdef";
    char* text = (char*)malloc(vga.xres * 2 + 1);
    if (!text) return;
    for (int y = 0; y < vga.yres; y++) {
        const uint8_t* src = vga.frameBuffer[y];
        for (int x = 0; x < vga.xres; x++) {
            uint8_t c = src[x ^ 2] & 0x3F;
            text[x * 2] = hex[c >> 4];
            text[x * 2 + 1] = hex[c & 15];
        }
        text[vga.xres * 2] = 0;
        printf("A2 fb %d %s\n", y, text);
    }
    free(text);
    printf("A2 fb end\n");
}

// Image BMP 8 bits de 560 x 384 points (chaque ligne deux fois, comme à l'écran)
bool screenshot(const char* path) {
    if (!ready) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const int W = A2_LINE_PIXELS, H = A2_VISIBLE_LINES * 2;
    const uint32_t paletteSize = 64 * 4, dataOffset = 54 + paletteSize, fileSize = dataOffset + W * H;
    uint8_t header[54] = { 'B', 'M' };
    auto put32 = [&](int pos, uint32_t v) { memcpy(header + pos, &v, 4); };
    put32(2, fileSize); put32(10, dataOffset); put32(14, 40); put32(18, W); put32(22, H);
    header[26] = 1; header[28] = 8;
    put32(34, W * H); put32(46, 64);
    fwrite(header, 1, sizeof(header), f);
    // Palette : les 64 couleurs de la sortie VGA (2 bits par composante)
    for (int c = 0; c < 64; c++) {
        uint8_t entry[4] = { (uint8_t)(((c >> 4) & 3) * 85), (uint8_t)(((c >> 2) & 3) * 85), (uint8_t)((c & 3) * 85), 0 };
        fwrite(entry, 1, 4, f);
    }
    uint8_t line[A2_LINE_PIXELS];
    bool ok = true;
    for (int y = H - 1; y >= 0 && ok; y--) {
        const uint8_t* src = vga.frameBuffer[y >> 1];
        for (int x = 0; x < W; x++) line[x] = src[x ^ 2] & 0x3F;
        ok = fwrite(line, 1, W, f) == (size_t)W;
    }
    fclose(f);
    return ok;
}

}

// Interruption I2S, à la fin de chaque descripteur DMA (une demi-ligne VGA).
// Elle signale la fin de la dernière ligne d'image : c'est le moment où la
// boucle principale lance l'image suivante, pour que le rendu reste toujours
// en avance sur le balayage de l'écran (pas de déchirure).
IRAM_ATTR void VGA6Bit::interrupt(void* arg) {
    if (Video::lastLineDescriptor && REG_READ(I2S_OUT_EOF_DES_ADDR_REG(1)) == Video::lastLineDescriptor)
        Video::vsync = true;
}

// Une ligne rendue -> framebuffer. Le rendu a déjà produit les octets VGA
// (A2::setPixelMap) ; il reste à les recopier par mots de 32 bits en échangeant
// leurs moitiés : dans le framebuffer, le point x est à l'octet x ^ 2.
IRAM_ATTR void A2_platformLine(int y, const uint8_t* px) {
    if (!Video::ready) return;
    uint32_t* dst = (uint32_t*)Video::vga.frameBuffer[y];
    const uint32_t* src = (const uint32_t*)px;
    for (int i = 0; i < A2_LINE_PIXELS / 4; i++) {
        uint32_t w = src[i];
        dst[i] = (w << 16) | (w >> 16);
    }
}
