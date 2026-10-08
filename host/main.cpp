/*

Apple2_ProjectESP32 — Banc de test sur PC du cœur d'émulation.

Compile les fichiers de Apple2_ProjectESP32/src/core tels quels, sans ESP32, et
les pilote par une suite d'actions données sur la ligne de commande, exécutées
dans l'ordre :

  model=N          0 : ][, 1 : ][+, 2 : //e, 3 : //e Enhanced, 4 : //c, 5 à 7 : //c ROM 0, 3, 4 (démarrage à froid)
  charset=N        jeu de caractères du //e et du //c : 0 américain, 1 français
  noaux            //e sans mémoire auxiliaire (à placer avant model=)
  disk1=FICHIER    insère une disquette (.dsk .do .po .nib .2mg) ; disk2= de même
  hdd=FICHIER      monte une image comme disque dur ProDOS en slot 7 (.hdv .po .2mg)
  boot             démarrage à froid
  reset            Ctrl-Reset
  run=N            émule N images
  type=TEXTE       tape le texte (\r : Retour, \e : Échap, \xNN : code)
  shot=FICHIER     écrit l'image courante en PNG (560 x 384)
  text             affiche l'écran texte 40 colonnes
  text80           affiche l'écran texte 80 colonnes
  mousecard=0|1    carte souris en slot 2 ; mouse=DX,DY : déplacement ; mbutton=0|1 : bouton
  serial           affiche ce que le port imprimante du //c a émis
  mono=0|1         rendu monochrome ; dhgrmono=0|1 : double haute résolution sans couleurs
  savestate=FICHIER, loadstate=FICHIER   sauvegarde et reprise de l'état de la machine
  wav=FICHIER      enregistre le son des images suivantes (fermé en fin de programme)
  save1=FICHIER    écrit l'image de la disquette 1 telle qu'elle est en mémoire
  state            affiche le 6502 et les soft switches
  mem=ADR,N        affiche N octets de mémoire (hexadécimal)
  bench=N          émule N images et donne la vitesse
  cputest=FICHIER,DEBUT,FIN[,c]   test de Klaus Dormann (c : 65C02)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include "A2.h"

using namespace std;

static uint8_t mainRam[0x10000], auxRam[0x10000];
static uint8_t frame[A2_VISIBLE_LINES][A2_LINE_PIXELS];

struct HostDisk {
    vector<uint8_t> data;
    bool dirty = false;
};
static HostDisk disks[2];

void A2_platformLine(int y, const uint8_t* pixels) { memcpy(frame[y], pixels, A2_LINE_PIXELS); }

bool A2_platformDiskRead(int drive, uint32_t offset, uint8_t* buf, uint32_t len) {
    if (offset + len > disks[drive].data.size()) return false;
    memcpy(buf, &disks[drive].data[offset], len);
    return true;
}

bool A2_platformDiskWrite(int drive, uint32_t offset, const uint8_t* buf, uint32_t len) {
    if (offset + len > disks[drive].data.size()) return false;
    memcpy(&disks[drive].data[offset], buf, len);
    disks[drive].dirty = true;
    return true;
}

// Disque dur : l'image entière en mémoire, les blocs après un éventuel en-tête 2MG
static vector<uint8_t> hddData;
static uint32_t hddOffset = 0;

bool A2_platformHddRead(uint32_t block, uint8_t* buf) {
    size_t pos = hddOffset + (size_t)block * 512;
    if (pos + 512 > hddData.size()) return false;
    memcpy(buf, &hddData[pos], 512);
    return true;
}

bool A2_platformHddWrite(uint32_t block, const uint8_t* buf) {
    size_t pos = hddOffset + (size_t)block * 512;
    if (pos + 512 > hddData.size()) return false;
    memcpy(&hddData[pos], buf, 512);
    return true;
}

// Ports série du //c : ce que le port 1 (imprimante) émet est gardé pour l'action serial
static string serialOut;
void A2_platformSerialOut(int port, uint8_t value) {
    if (port == 0) serialOut += (char)(value & 0x7F);
}

static bool stateWrite(void* ctx, void* data, uint32_t len) { return fwrite(data, 1, len, (FILE*)ctx) == len; }
static bool stateRead(void* ctx, void* data, uint32_t len) { return fread(data, 1, len, (FILE*)ctx) == len; }

static bool readFile(const string& path, vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size);
    bool ok = fread(out.data(), 1, size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static string lowerExt(const string& path) {
    size_t dot = path.find_last_of('.');
    string ext = dot == string::npos ? "" : path.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

// La reconnaissance du format est celle du firmware (A2::Disk::identify)
static bool insertDisk(int drive, const string& path) {
    HostDisk& d = disks[drive];
    if (!readFile(path, d.data)) { printf("[!] illisible : %s\n", path.c_str()); return false; }
    A2::Disk::Format fmt;
    int tracks;
    uint32_t offset;
    A2::Disk::IdentifyResult error = A2::Disk::identify(lowerExt(path).c_str(), (uint32_t)d.data.size(), d.data.data(),
                                                        (uint32_t)min<size_t>(d.data.size(), 2048), &fmt, &tracks, &offset);
    if (error != A2::Disk::ID_OK || fmt == A2::Disk::FMT_HDD) {
        printf("[!] %s : %s\n", path.c_str(), error == A2::Disk::ID_BAD_2MG ? "en-tete 2MG invalide"
               : error == A2::Disk::ID_BAD_SIZE ? "taille d'image non reconnue" : "image de disque dur (action hdd=)");
        d.data.clear();
        A2::Disk::eject(drive);
        return false;
    }
    A2::Disk::insert(drive, fmt, tracks, offset, false);
    printf("[+] lecteur %d : %s (%d pistes, format %d)\n", drive + 1, path.c_str(), tracks, (int)fmt);
    return true;
}

// --- PNG sans compression ----------------------------------------------------

static uint32_t crcTable[256];
static void crcInit() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    crc = ~crc;
    while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static void put32(vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE* f, const char* type, const vector<uint8_t>& body) {
    vector<uint8_t> c;
    put32(c, (uint32_t)body.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), body.begin(), body.end());
    put32(c, crc32(c.data() + 4, c.size() - 4));
    fwrite(c.data(), 1, c.size(), f);
}

static bool writePng(const string& path) {
    const int W = A2_LINE_PIXELS, H = A2_VISIBLE_LINES * 2;
    vector<uint8_t> raw;
    raw.reserve((size_t)H * (W * 3 + 1));
    for (int y = 0; y < H; y++) {
        raw.push_back(0);
        for (int x = 0; x < W; x++) {
            const uint8_t* c = A2::paletteRGB[frame[y / 2][x] & 15];
            raw.push_back(c[0]); raw.push_back(c[1]); raw.push_back(c[2]);
        }
    }
    // zlib « stocké » : blocs de 65535 octets au plus
    vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[pos + i]) % 65521;
            b = (b + a) % 65521;
        }
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put32(z, (b << 16) | a);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    vector<uint8_t> ihdr;
    put32(ihdr, W); put32(ihdr, H);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

// --- Écran texte -------------------------------------------------------------

static void dumpText(bool col80) {
    char line[81];
    for (int row = 0; row < 24; row++) {
        A2::textScreenLine(row, col80, line);
        printf("|%s\n", line);
    }
}

// --- Actions -----------------------------------------------------------------

static FILE* wavFile = nullptr;
static uint32_t wavBytes = 0;
#define WAV_SAMPLES 520          // par image : 31 158 Hz

static void wavHeader(FILE* f, uint32_t dataBytes) {
    uint32_t rate = WAV_SAMPLES * 5992 / 100;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                      0,0,0,0, 0,0,0,0, 1,0, 8,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t riff = dataBytes + 36;
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &rate, 4); memcpy(h + 40, &dataBytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void runFrames(int n) {
    for (int i = 0; i < n; i++) {
        A2::runFrame();
        if (wavFile) {
            uint8_t samples[WAV_SAMPLES];
            A2::renderAudio(samples, WAV_SAMPLES, 4);
            fwrite(samples, 1, WAV_SAMPLES, wavFile);
            wavBytes += WAV_SAMPLES;
        }
    }
}

static void typeText(const string& text) {
    vector<char> keys(text.size() + 1);
    int count = A2::unescape(text.c_str(), keys.data(), (int)keys.size());
    for (int i = 0; i < count; i++) {
        uint8_t c = (uint8_t)keys[i];
        // Attend que le programme ait lu la touche précédente
        for (int guard = 0; guard < 600 && A2::keyWaiting(); guard++) runFrames(1);
        A2::keyDown(c);
        runFrames(3);
        A2::keyUp();
        runFrames(2);
    }
}

static void printState() {
    printf("PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X  sw=%05X  cycles=%u images=%u  disque: %u pistes lues, %u ecrites\n",
           A2::cpu.pc, A2::cpu.a, A2::cpu.x, A2::cpu.y, A2::cpu.sp, A2::cpu.p, (unsigned)A2::sw,
           (unsigned)A2::cycles, (unsigned)A2::frameCount, (unsigned)A2::Disk::statReads, (unsigned)A2::Disk::statWrites);
    printf("  tete : lecteur 1 piste %.2f, lecteur 2 piste %.2f, moteur %s\n", A2::Disk::quarterTrack(0) / 4.0,
           A2::Disk::quarterTrack(1) / 4.0, A2::Disk::spinning() ? "en marche" : "arrete");
}

// Tests de Klaus Dormann : une image mémoire de 64 Ko, le test tourne jusqu'à
// boucler sur lui-même. L'adresse de la boucle dit s'il a réussi.
static int cpuTest(const string& spec) {
    vector<string> parts;
    size_t start = 0;
    for (size_t comma; (comma = spec.find(',', start)) != string::npos; start = comma + 1)
        parts.push_back(spec.substr(start, comma - start));
    parts.push_back(spec.substr(start));
    if (parts.size() < 3) { printf("cputest=FICHIER,DEBUT,FIN[,c]\n"); return 1; }
    vector<uint8_t> image;
    if (!readFile(parts[0], image) || image.size() != 0x10000) { printf("[!] image de 64 Ko attendue\n"); return 1; }
    static uint8_t flat[0x10000];
    memcpy(flat, image.data(), 0x10000);
    for (int page = 0; page < 256; page++) A2::rdPage[page] = A2::wrPage[page] = flat + (page << 8);
    uint16_t entry = (uint16_t)strtol(parts[1].c_str(), nullptr, 16);
    uint16_t success = (uint16_t)strtol(parts[2].c_str(), nullptr, 16);
    A2::cpu.cmos = parts.size() > 3 && parts[3] == "c";
    A2::cpu.pc = entry;
    A2::cpu.p = 0x24;
    A2::cpu.sp = 0xFF;
    uint16_t last = 0xFFFF;
    for (uint64_t steps = 0; steps < 200000000ULL; steps++) {
        // Une instruction : cpuRun reporte les cycles exécutés en trop, et peut donc ne rien faire
        uint32_t before = A2::cycles;
        while (A2::cycles == before) A2::cpuRun(1);
        if (A2::cpu.pc == last) {
            bool ok = last == success;
            printf("[%s] %s (%s) : boucle en $%04X apres %llu instructions, %u cycles\n", ok ? "OK" : "ECHEC",
                   parts[0].c_str(), A2::cpu.cmos ? "65C02" : "6502", last, (unsigned long long)steps, (unsigned)A2::cycles);
            return ok ? 0 : 1;
        }
        last = A2::cpu.pc;
    }
    printf("[ECHEC] pas de boucle finale, PC=$%04X\n", A2::cpu.pc);
    return 1;
}

int main(int argc, char** argv) {
    crcInit();
    bool useAux = true;
    bool started = false;
    int rc = 0;
    auto start = [&]() {
        if (!started) {
            A2::init(mainRam, useAux ? auxRam : nullptr);
            static uint8_t expansion[0x100000];
            A2::setExpansionRam(expansion, sizeof(expansion));
            started = true;
        }
    };
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        size_t eq = arg.find('=');
        string key = arg.substr(0, eq);
        string val = eq == string::npos ? "" : arg.substr(eq + 1);
        if (key == "cputest") { rc |= cpuTest(val); continue; }
        if (key == "noaux") { useAux = false; continue; }
        start();
        if (key == "model") A2::setModel((A2::Model)atoi(val.c_str()));
        else if (key == "disk1") insertDisk(0, val);
        else if (key == "disk2") insertDisk(1, val);
        else if (key == "hdd") {
            // Monte l'image comme disque dur, quelle que soit sa taille
            if (!readFile(val, hddData)) { printf("[!] illisible : %s\n", val.c_str()); continue; }
            hddOffset = (hddData.size() >= 64 && memcmp(hddData.data(), "2IMG", 4) == 0)
                      ? (hddData[24] | (hddData[25] << 8) | (hddData[26] << 16)) : 0;
            uint32_t blocks = (uint32_t)((hddData.size() - hddOffset) / 512);
            A2::Hdd::insert(blocks, false);
            printf("[+] disque dur : %s (%u blocs)\n", val.c_str(), blocks);
        }
        else if (key == "boot") A2::powerOn();
        else if (key == "reset") A2::reset();
        else if (key == "run") runFrames(atoi(val.c_str()));
        else if (key == "type") typeText(val);
        else if (key == "shot") { if (!writePng(val)) printf("[!] ecriture impossible : %s\n", val.c_str()); }
        else if (key == "text") dumpText(false);
        else if (key == "text80") dumpText(true);
        else if (key == "mousecard") A2::Mouse::setEnabled(atoi(val.c_str()) != 0);
        else if (key == "mouse") {
            // Déplacement DX,DY en plusieurs images, comme une vraie souris
            int dx = atoi(val.c_str()), dy = 0;
            size_t comma = val.find(',');
            if (comma != string::npos) dy = atoi(val.c_str() + comma + 1);
            for (int step = 0; step < 16; step++) {
                A2::Mouse::move(dx * (step + 1) / 16 - dx * step / 16, dy * (step + 1) / 16 - dy * step / 16);
                runFrames(1);
            }
        }
        else if (key == "mbutton") { A2::Mouse::setButton(0, atoi(val.c_str()) != 0); runFrames(4); }
        else if (key == "charset") A2::setCharset((uint8_t)atoi(val.c_str()));
        else if (key == "serial") {
            // Affiche ce que le port imprimante du //c a émis
            for (char& ch : serialOut) if (ch == 0x0D) ch = '\n';
            printf("[port 1] %s\n", serialOut.c_str());
            serialOut.clear();
        }
        else if (key == "mono") A2::monochrome = atoi(val.c_str()) != 0;
        else if (key == "dhgrmono") A2::dhiresMono = atoi(val.c_str()) != 0;
        else if (key == "savestate" || key == "loadstate") {
            bool saving = key == "savestate";
            FILE* f = fopen(val.c_str(), saving ? "wb" : "rb");
            bool ok = f != nullptr;
            if (ok) {
                ok = saving ? A2::saveState(stateWrite, f) : A2::loadState(stateRead, f);
                fclose(f);
            }
            printf("[%s] %s %s\n", ok ? "+" : "!", key.c_str(), val.c_str());
        }
        else if (key == "state") printState();
        else if (key == "mem") {
            int addr = (int)strtol(val.c_str(), nullptr, 16);
            size_t comma = val.find(',');
            int n = comma == string::npos ? 16 : (int)strtol(val.c_str() + comma + 1, nullptr, 16);
            for (int i = 0; i < n; i++) {
                if (i % 16 == 0) printf("%s%04X:", i ? "\n" : "", (addr + i) & 0xFFFF);
                printf(" %02X", A2::memRead((uint16_t)(addr + i)));
            }
            printf("\n");
        }
        else if (key == "wav") { wavFile = fopen(val.c_str(), "wb"); if (wavFile) wavHeader(wavFile, 0); }
        else if (key == "save1") {
            A2::Disk::flush();
            FILE* f = fopen(val.c_str(), "wb");
            if (f) { fwrite(disks[0].data.data(), 1, disks[0].data.size(), f); fclose(f); }
        }
        else if (key == "bench") {
            int n = atoi(val.c_str());
            auto t0 = chrono::steady_clock::now();
            runFrames(n);
            double s = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            printf("%d images en %.3f s : %.0f images/s, %.1f MHz de 6502\n", n, s, n / s, n / s * A2_CYCLES_PER_FRAME / 1e6);
        }
        else { printf("[!] action inconnue : %s\n", arg.c_str()); rc = 1; }
    }
    if (wavFile) { wavHeader(wavFile, wavBytes); fclose(wavFile); }
    return rc;
}
