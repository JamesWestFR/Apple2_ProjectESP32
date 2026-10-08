/*

Apple2_ProjectESP32 — Console de diagnostic sur le port série.

Permet de piloter et d'observer l'émulateur sans écran ni clavier, depuis
scripts/a2console.py. Une commande par ligne :

  s            état : 6502, soft switches, vitesse, lecteurs, mémoire libre
  t            écran texte 40 colonnes de l'Apple (page 1) ; t80 : en 80 colonnes
  f            points allumés du framebuffer VGA, par ligne de texte
  p            tout le framebuffer, en hexadécimal (a2console.py : shot FICHIER.png)
  k TEXTE      tape le texte (\r : Retour, \e : Échap, \xNN : code)
  d1 CHEMIN    insère une disquette dans le lecteur 1 (d2 : lecteur 2)
  e1 / e2      éjecte ; eh : retire le disque dur (une grande image passée à d1 y est montée)
  b            redémarrage à froid de l'Apple
  r            Ctrl-Reset
  m N          modèle (0 : ][, 1 : ][+, 2 : //e, 3 : //e Enhanced, 4 : //c, 5 à 7 : //c ROM 0, 3, 4, 8 : //c Plus)
  cs 0|1       caractères américains ou français
  x            capture d'écran sur la carte SD
  ls CHEMIN    contenu d'un dossier de la carte SD
  ss / sl      sauvegarde / reprise de l'état (Ctrl+F9 / Ctrl+F10)
  u            vitesse maximale, oui ou non
  mc 0|1       carte souris absente ou présente ; mm DX DY : déplacement ; mb 0|1 : bouton
  i            redessine toute l'image à chaque fois, oui ou non (mesure du pire cas)
  menu         ouvre ou ferme le menu (comme F12) ; files : le choix d'une disquette (F11)
  vk TOUCHE    une touche dans le menu : up, down, left, right, enter, esc, pgdn, space, ou un caractère
  put N CHEMIN reçoit un fichier de N octets, en hexadécimal, et l'écrit sur la SD
  mkfile MO CHEMIN   crée un fichier de test ; rm CHEMIN le supprime
  hddbench     temps de lecture des blocs du disque dur monté

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "SerialConsole.h"
#include "Emu.h"
#include "ESPConfig.h"
#include "Video.h"
#include "DiskImage.h"
#include "OSD.h"
#include "core/A2.h"

using namespace std;

namespace SerialConsole {

static char line[160];
static int lineLen = 0;

// Texte en attente de frappe : une touche à la fois, quand l'Apple a lu la précédente
static string typeQueue;
static int keyHold = 0;

static void printText(bool col80) {
    char text[81];
    for (int row = 0; row < 24; row++) {
        A2::textScreenLine(row, col80, text);
        printf("|%s\n", text);
    }
}

static void printState() {
    printf("A2 %s PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X sw=%05X frames=%u\n", A2::modelName(A2::model()),
           A2::cpu.pc, A2::cpu.a, A2::cpu.x, A2::cpu.y, A2::cpu.sp, A2::cpu.p, (unsigned)A2::sw, (unsigned)A2::frameCount);
    printf("A2 speed: %.1f images/s, %.1f emulated/s, %.2f ms/image (budget %.2f)\n", Emu::statFps, Emu::statEmulatedFps,
           Emu::statFrameMs, MICROS_PER_FRAME / 1000.0);
    for (int d = 0; d < 2; d++)
        printf("A2 drive %d: %s, track %.2f\n", d + 1, DiskImage::path(d).empty() ? "(empty)" : DiskImage::path(d).c_str(),
               A2::Disk::quarterTrack(d) / 4.0);
    printf("A2 hard disk: %s, %u blocks read, %u written\n", DiskImage::hddPath().empty() ? "(none)" : DiskImage::hddPath().c_str(),
           (unsigned)A2::Hdd::statReads, (unsigned)A2::Hdd::statWrites);
    printf("A2 disk: %u tracks read, %u written, motor %s\n", (unsigned)A2::Disk::statReads, (unsigned)A2::Disk::statWrites,
           A2::Disk::spinning() ? "on" : "off");
    printf("A2 heap: internal %u KB free (largest %u), PSRAM %u KB free\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void queueText(const char* text) {
    char keys[160];
    int count = A2::unescape(text, keys, sizeof(keys));
    typeQueue.append(keys, count);
}

// put TAILLE CHEMIN : reçoit un fichier en hexadécimal (le port série convertit
// les fins de ligne, le binaire brut n'y passerait pas) et l'écrit sur la
// carte SD. L'émulation est suspendue pendant le transfert.
static void receiveFile(char* arg) {
    uint32_t size = (uint32_t)strtoul(arg, &arg, 10);
    while (*arg == ' ') arg++;
    if (size == 0 || size > 2 * 1024 * 1024 || !*arg) { printf("A2 put: usage put SIZE PATH\n"); return; }
    uint8_t* data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!data) { printf("A2 put: no memory\n"); return; }
    printf("A2 put ready\n");
    fflush(stdout);

    uint32_t received = 0, sum = 0;
    int high = -1;
    int64_t last = esp_timer_get_time();
    while (received < size && esp_timer_get_time() - last < 5000000) {
        int c = fgetc(stdin);
        if (c == EOF) continue;
        last = esp_timer_get_time();
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (high < 0) { high = v; continue; }
        data[received] = (uint8_t)((high << 4) | v);
        sum += data[received++];
        high = -1;
    }
    bool ok = received == size;
    if (ok) {
        FILE* f = fopen(arg, "wb");
        ok = f && fwrite(data, 1, size, f) == size;
        if (f) fclose(f);
    }
    heap_caps_free(data);
    printf("A2 put %s: %u bytes, sum %u\n", ok ? "ok" : "FAILED", (unsigned)received, (unsigned)sum);
}

// Lit `len` octets bruts dans le tampon matériel de l'UART (sans passer par
// stdin, qui convertit les fins de ligne). Faux si rien n'arrive pendant 3 s.
static bool uartReadRaw(uint8_t* buf, uint32_t len) {
    uint32_t got = 0;
    int64_t last = esp_timer_get_time();
    while (got < len) {
        uint32_t n = uart_ll_get_rxfifo_len(&UART0);
        if (n == 0) {
            if (esp_timer_get_time() - last > 3000000) return false;
            continue;
        }
        if (n > len - got) n = len - got;
        uart_ll_read_rxfifo(&UART0, buf + got, n);
        got += n;
        last = esp_timer_get_time();
    }
    return true;
}

static void uartWriteRaw(char c) {
    uart_ll_write_txfifo(&UART0, (const uint8_t*)&c, 1);
}

// putbig TAILLE DEBIT CHEMIN : reçoit un grand fichier en binaire, par blocs de
// 4 Ko vérifiés un à un (0xA5, les données, leur somme sur 32 bits ; réponse
// K ou N, et le bloc est renvoyé après un N). Le bloc est écrit sur la carte
// SD avant la réponse : l'émetteur attend, rien ne se perd pendant l'écriture.
// Le port passe au débit demandé pendant le transfert.
#define BIG_CHUNK 4096
static void receiveBigFile(char* arg) {
    uint32_t size = (uint32_t)strtoul(arg, &arg, 10);
    uint32_t baud = (uint32_t)strtoul(arg, &arg, 10);
    while (*arg == ' ') arg++;
    if (size == 0 || baud < 9600 || !*arg) { printf("A2 putbig: usage putbig SIZE BAUD PATH\n"); return; }
    uint8_t* chunk = (uint8_t*)malloc(BIG_CHUNK + 4);
    FILE* f = fopen(arg, "wb");
    if (!chunk || !f) { printf("A2 putbig: FAILED to open\n"); if (f) fclose(f); free(chunk); return; }
    printf("A2 putbig ready\n");
    fflush(stdout);
    uart_wait_tx_idle_polling(UART_NUM_0);
    uart_set_baudrate(UART_NUM_0, baud);

    uint32_t received = 0, retries = 0;
    bool ok = true;
    Video::overlayText(0, 0, T(" TRANSFERT... ", " TRANSFER... "));
    while (ok && received < size) {
        uint32_t len = size - received > BIG_CHUNK ? BIG_CHUNK : size - received;
        uint8_t mark = 0;
        bool good = uartReadRaw(&mark, 1) && mark == 0xA5 && uartReadRaw(chunk, len + 4);
        if (good) {
            uint32_t sum = 0;
            for (uint32_t i = 0; i < len; i++) sum += chunk[i];
            uint32_t sent = chunk[len] | (chunk[len + 1] << 8) | (chunk[len + 2] << 16) | ((uint32_t)chunk[len + 3] << 24);
            good = sum == sent;
        }
        if (!good) {
            // Bloc abîmé : attend le silence, vide le tampon et le redemande
            if (++retries > 50) { ok = false; break; }
            int64_t quiet = esp_timer_get_time();
            while (esp_timer_get_time() - quiet < 200000) {
                uint8_t junk[64];
                uint32_t n = uart_ll_get_rxfifo_len(&UART0);
                if (n) { uart_ll_read_rxfifo(&UART0, junk, n > 64 ? 64 : n); quiet = esp_timer_get_time(); }
            }
            uartWriteRaw('N');
            continue;
        }
        if (fwrite(chunk, 1, len, f) != len) { ok = false; break; }
        received += len;
        if ((received & 0xFFFFF) < BIG_CHUNK) {
            char text[24];
            snprintf(text, sizeof(text), " %u / %u Mo ", (unsigned)(received >> 20), (unsigned)(size >> 20));
            Video::overlayText(0, 0, text);
        }
        uartWriteRaw('K');
    }
    if (!ok) uartWriteRaw('X');
    fclose(f);
    free(chunk);
    // Un fichier incomplet ne doit pas passer pour une image valide
    if (!ok) remove(arg);
    uart_wait_tx_idle_polling(UART_NUM_0);
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_set_baudrate(UART_NUM_0, 115200);
    vTaskDelay(pdMS_TO_TICKS(300));
    printf("A2 putbig %s: %u bytes, %u retries\n", ok ? "ok" : "FAILED", (unsigned)received, (unsigned)retries);
    A2::videoInvalidate();
}

// mkfile MO CHEMIN : crée un fichier de la taille d'un volume ProDOS (au plus
// 32 Mo), pour mesurer les accès à une grande image sans devoir l'envoyer
static void makeFile(char* arg) {
    uint32_t megabytes = (uint32_t)strtoul(arg, &arg, 10);
    while (*arg == ' ') arg++;
    if (megabytes == 0 || megabytes > 32 || !*arg) { printf("A2 mkfile: usage mkfile MB PATH\n"); return; }
    uint32_t blocks = megabytes * 2048;
    if (blocks > 65535) blocks = 65535;
    FILE* f = fopen(arg, "wb");
    uint8_t* buf = (uint8_t*)malloc(4096);
    if (!f || !buf) { printf("A2 mkfile: FAILED\n"); if (f) fclose(f); free(buf); return; }
    int64_t t0 = esp_timer_get_time();
    uint32_t written = 0;
    bool ok = true;
    while (ok && written < blocks) {
        uint32_t n = blocks - written > 8 ? 8 : blocks - written;
        // Chaque bloc porte son numéro : la relecture peut être vérifiée
        for (uint32_t b = 0; b < n; b++) {
            memset(buf + b * 512, (int)((written + b) & 0xFF), 512);
            buf[b * 512] = (uint8_t)((written + b) >> 8);
        }
        ok = fwrite(buf, 512, n, f) == n;
        written += n;
    }
    fclose(f);
    free(buf);
    printf("A2 mkfile %s: %u blocks in %.1f s\n", ok ? "ok" : "FAILED", (unsigned)written, (esp_timer_get_time() - t0) / 1e6);
}

// hddbench : temps de lecture de blocs du disque dur monté, à la suite puis au hasard
static void hddBench() {
    if (!A2::Hdd::inserted()) { printf("A2 hddbench: no hard disk\n"); return; }
    uint8_t buf[512];
    uint32_t blocks = A2::Hdd::blockCount();
    int errors = 0;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t b = 0; b < 200; b++) errors += !A2_platformHddRead(b % blocks, buf);
    int64_t t1 = esp_timer_get_time();
    uint32_t seed = 12345;
    int64_t worst = 0;
    for (int i = 0; i < 200; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t b = (seed >> 8) % blocks;
        int64_t s = esp_timer_get_time();
        bool ok = A2_platformHddRead(b, buf);
        int64_t d = esp_timer_get_time() - s;
        if (d > worst) worst = d;
        if (!ok) errors++;
    }
    int64_t t2 = esp_timer_get_time();
    printf("A2 hddbench: %u blocks, sequential %.2f ms/block, random %.2f ms/block (worst %.1f), %d errors\n",
           (unsigned)blocks, (t1 - t0) / 200000.0, (t2 - t1) / 200000.0, worst / 1000.0, errors);
}

static void execute(char* cmd) {
    while (*cmd == ' ') cmd++;
    char* arg = strchr(cmd, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = cmd + strlen(cmd);

    if (!strcmp(cmd, "put")) { receiveFile(arg); return; }
    if (!strcmp(cmd, "putbig")) { receiveBigFile(arg); return; }
    if (!strcmp(cmd, "mkfile")) { makeFile(arg); return; }
    if (!strcmp(cmd, "ls")) {
        // ls CHEMIN : contenu d'un dossier de la carte SD (la racine par défaut)
        string dirPath = *arg ? arg : "/sd";
        DIR* dir = opendir(dirPath.c_str());
        if (!dir) { printf("A2 ls %s: FAILED\n", dirPath.c_str()); return; }
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            struct stat st;
            string full = dirPath + "/" + de->d_name;
            long size = (de->d_type != DT_DIR && stat(full.c_str(), &st) == 0) ? (long)st.st_size : -1;
            if (size < 0) printf("A2 ls      <dir> %s\n", de->d_name);
            else printf("A2 ls %10ld %s\n", size, de->d_name);
        }
        closedir(dir);
        printf("A2 done ls\n");
        return;
    }
    if (!strcmp(cmd, "hddbench")) { hddBench(); return; }
    if (!strcmp(cmd, "rm")) { printf("A2 rm %s: %s\n", arg, remove(arg) == 0 ? "ok" : "FAILED"); return; }

    if (!strcmp(cmd, "s")) printState();
    else if (!strcmp(cmd, "t")) printText(false);
    else if (!strcmp(cmd, "t80")) printText(true);
    else if (!strcmp(cmd, "f")) Video::printSummary();
    else if (!strcmp(cmd, "p")) Video::dumpFramebuffer();
    else if (!strcmp(cmd, "k")) queueText(arg);
    else if (!strcmp(cmd, "d1") || !strcmp(cmd, "d2")) {
        int d = cmd[1] - '1';
        bool ok = DiskImage::insert(d, arg);
        printf("A2 insert %d: %s\n", d + 1, ok ? "ok" : DiskImage::lastError.c_str());
        if (ok && DiskImage::lastWasHdd) Config::hdd = arg;
        else if (ok) Config::disk[d] = arg;
        Config::save();
    }
    else if (!strcmp(cmd, "e1") || !strcmp(cmd, "e2")) { DiskImage::eject(cmd[1] - '1'); Config::disk[cmd[1] - '1'] = ""; Config::save(); }
    else if (!strcmp(cmd, "eh")) { DiskImage::ejectHdd(); Config::hdd = ""; Config::save(); }
    else if (!strcmp(cmd, "b")) Emu::coldBootRequest = true;
    else if (!strcmp(cmd, "r")) Emu::resetRequest = true;
    else if (!strcmp(cmd, "m")) {
        int m = atoi(arg);
        if (m >= 0 && m < A2::MODEL_COUNT) {
            Config::model = (uint8_t)m;
            Config::save();
            A2::Disk::flush();
            A2::setModel((A2::Model)m);
        }
    }
    else if (!strcmp(cmd, "x")) Emu::screenshotRequest = true;
    else if (!strcmp(cmd, "ss")) Emu::stateRequest = 1;
    else if (!strcmp(cmd, "sl")) Emu::stateRequest = 2;
    else if (!strcmp(cmd, "menu")) OSD::toggle(OSD::MENU_MAIN);
    else if (!strcmp(cmd, "files")) OSD::toggle(OSD::MENU_FILES);
    else if (!strcmp(cmd, "vk")) {
        // Une touche du menu : up, down, left, right, enter, esc, pgdn, ou un caractère
        fabgl::VirtualKey vk = fabgl::VK_NONE;
        uint8_t ascii = 0;
        if (!strcmp(arg, "up")) vk = fabgl::VK_UP;
        else if (!strcmp(arg, "down")) vk = fabgl::VK_DOWN;
        else if (!strcmp(arg, "left")) vk = fabgl::VK_LEFT;
        else if (!strcmp(arg, "right")) vk = fabgl::VK_RIGHT;
        else if (!strcmp(arg, "enter")) vk = fabgl::VK_RETURN;
        else if (!strcmp(arg, "esc")) vk = fabgl::VK_ESCAPE;
        else if (!strcmp(arg, "pgdn")) vk = fabgl::VK_PAGEDOWN;
        else if (!strcmp(arg, "space")) ascii = ' ';
        else ascii = (uint8_t)arg[0];
        OSD::key(vk, ascii);
    }
    else if (!strcmp(cmd, "mm")) {
        // mm DX DY : déplacement de la souris ; mb 0|1 : son bouton
        char* end;
        int dx = (int)strtol(arg, &end, 10), dy = (int)strtol(end, nullptr, 10);
        A2::Mouse::move(dx, dy);
    }
    else if (!strcmp(cmd, "mb")) A2::Mouse::setButton(0, atoi(arg) != 0);
    else if (!strcmp(cmd, "mc")) { Config::mouse = atoi(arg) != 0; Config::save(); A2::Mouse::setEnabled(Config::mouse); }
    else if (!strcmp(cmd, "cs")) { Config::charset = atoi(arg) != 0; Config::save(); Video::setMonitor(Config::monitor); }
    else if (!strcmp(cmd, "u")) Emu::turbo = !Emu::turbo;
    else if (!strcmp(cmd, "i")) Emu::forceRedraw = !Emu::forceRedraw;
    else if (*cmd) printf("A2 ? %s\n", cmd);
    if (strcmp(cmd, "k")) printf("A2 done %s\n", cmd);
}

void tick() {
    // Sans pilote UART installé, la lecture de stdin ne bloque pas
    for (int guard = 0; guard < 64; guard++) {
        int c = fgetc(stdin);
        if (c == EOF) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[lineLen] = 0;
            lineLen = 0;
            execute(line);
        } else if (lineLen < (int)sizeof(line) - 1) {
            line[lineLen++] = (char)c;
        }
    }

    // Frappe : appui pendant 3 images, puis attente que l'Apple ait lu la touche
    if (keyHold > 0) {
        if (--keyHold == 0) A2::keyUp();
    } else if (!typeQueue.empty() && !A2::keyWaiting()) {
        A2::keyDown((uint8_t)typeQueue[0]);
        typeQueue.erase(0, 1);
        keyHold = 3;
        if (typeQueue.empty()) printf("A2 done k\n");
    }
}

}
