/*

Apple2_ProjectESP32 — Images de disquette sur la carte SD.

L'image insérée est copiée en entier en PSRAM (140 Ko) : les lectures du
lecteur émulé ne touchent plus la carte SD. Sans PSRAM, chaque secteur est lu
dans le fichier. Les écritures vont dans la copie et dans le fichier, qui
reste ouvert tant que la disquette est dans le lecteur.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "DiskImage.h"
#include "FileUtils.h"
#include "ESPConfig.h"
#include "core/A2.h"

namespace DiskImage {

string lastError;

struct Slot {
    string path;
    FILE* file = nullptr;
    uint8_t* cache = nullptr;     // copie du fichier en PSRAM, ou nul
    uint32_t size = 0;
    bool readOnly = false;
};

static Slot slots[2];
static Slot hdd;                  // disque dur du slot 7
static uint32_t hddOffset = 0;    // position du bloc 0 dans le fichier (en-tête 2MG)
bool lastWasHdd = false;

static void release(Slot& s) {
    if (s.file) fclose(s.file);
    if (s.cache) heap_caps_free(s.cache);
    s = Slot();
}

void ejectHdd() {
    A2::Hdd::eject();
    release(hdd);
}

const string& hddPath() { return hdd.path; }

string baseName(const string& path) {
    size_t slash = path.find_last_of('/');
    return slash == string::npos ? path : path.substr(slash + 1);
}

string hddName() { return baseName(hdd.path); }

void eject(int drive) {
    drive &= 1;
    A2::Disk::eject(drive);     // écrit d'abord la piste en attente
    release(slots[drive]);
}

bool insert(int drive, const string& path) {
    drive &= 1;
    lastError = "";
    lastWasHdd = false;

    // Deux descripteurs ouverts en écriture sur le même fichier l'abîmeraient
    if (slots[drive ^ 1].path == path) { lastError = T("Deja dans l'autre lecteur", "Already in the other drive"); return false; }

    struct stat st;
    if (stat(path.c_str(), &st) != 0) { lastError = T("Fichier introuvable", "File not found"); return false; }
    uint32_t size = (uint32_t)st.st_size;

    Slot s;
    s.file = fopen(path.c_str(), "r+b");
    if (!s.file) {
        // Fichier en lecture seule : la disquette est protégée en écriture
        s.file = fopen(path.c_str(), "rb");
        s.readOnly = true;
    }
    if (!s.file) { lastError = T("Ouverture impossible", "Cannot open the file"); return false; }

    // Début du fichier, pour reconnaître le format (sur le tas : la pile de la
    // tâche principale est petite)
    const uint32_t headSize = 2048;
    uint8_t* head = (uint8_t*)malloc(headSize);
    if (!head) { lastError = T("Memoire insuffisante", "Out of memory"); fclose(s.file); return false; }
    uint32_t headLen = (uint32_t)fread(head, 1, headSize, s.file);
    A2::Disk::Format fmt;
    int tracks;
    uint32_t dataOffset;
    A2::Disk::IdentifyResult result = A2::Disk::identify(FileUtils::getLCaseExt(path).c_str(), size, head, headLen,
                                                         &fmt, &tracks, &dataOffset);
    free(head);
    if (result != A2::Disk::ID_OK) {
        lastError = result == A2::Disk::ID_BAD_2MG ? T("En-tete 2MG invalide", "Bad 2MG header")
                                                   : T("Taille d'image non reconnue", "Unknown image size");
        fclose(s.file);
        return false;
    }

    lastWasHdd = fmt == A2::Disk::FMT_HDD;
    if (!lastWasHdd) eject(drive);
    if (fmt == A2::Disk::FMT_HDD) {
        // Image par blocs : elle va dans le disque dur du slot 7, quel que soit
        // le lecteur demandé. Elle reste sur la carte SD, lue bloc par bloc.
        ejectHdd();
        s.path = path;
        s.size = size;
        hdd = s;
        hddOffset = dataOffset;
        A2::Hdd::insert((uint32_t)tracks, s.readOnly);
        printf("| Apple2_ProjectESP32: hard disk = %s (%d blocks%s)\n", path.c_str(), tracks, s.readOnly ? ", read only" : "");
        return true;
    }

    s.path = path;
    s.size = size;
    s.cache = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (s.cache) {
        fseek(s.file, 0, SEEK_SET);
        if (fread(s.cache, 1, size, s.file) != size) {
            heap_caps_free(s.cache);
            s.cache = nullptr;
        }
    }
    slots[drive] = s;
    A2::Disk::insert(drive, fmt, tracks, dataOffset, s.readOnly);
    printf("| Apple2_ProjectESP32: drive %d = %s (%d tracks, format %d%s%s)\n", drive + 1, path.c_str(), tracks,
           (int)fmt, s.readOnly ? ", read only" : "", s.cache ? ", cached" : "");
    return true;
}

void swap() {
    string a = slots[0].path, b = slots[1].path;
    eject(0);
    eject(1);
    if (!b.empty()) insert(0, b);
    if (!a.empty()) insert(1, a);
}

const string& path(int drive) { return slots[drive & 1].path; }

string name(int drive) { return baseName(slots[drive & 1].path); }

}

static bool readAt(FILE* f, uint32_t offset, uint8_t* buf, uint32_t len) {
    return f && fseek(f, offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

static bool writeAt(FILE* f, uint32_t offset, const uint8_t* buf, uint32_t len) {
    if (!f || fseek(f, offset, SEEK_SET) != 0) return false;
    bool ok = fwrite(buf, 1, len, f) == len;
    // Sans cela, les données restent en mémoire jusqu'à la fermeture du fichier
    fflush(f);
    fsync(fileno(f));
    return ok;
}

bool A2_platformDiskRead(int drive, uint32_t offset, uint8_t* buf, uint32_t len) {
    DiskImage::Slot& s = DiskImage::slots[drive & 1];
    if (offset + len > s.size) return false;
    if (s.cache) {
        memcpy(buf, s.cache + offset, len);
        return true;
    }
    return readAt(s.file, offset, buf, len);
}

bool A2_platformHddRead(uint32_t block, uint8_t* buf) {
    uint32_t offset = DiskImage::hddOffset + block * 512;
    if (offset + 512 > DiskImage::hdd.size) return false;
    return readAt(DiskImage::hdd.file, offset, buf, 512);
}

bool A2_platformHddWrite(uint32_t block, const uint8_t* buf) {
    uint32_t offset = DiskImage::hddOffset + block * 512;
    if (DiskImage::hdd.readOnly || offset + 512 > DiskImage::hdd.size) return false;
    return writeAt(DiskImage::hdd.file, offset, buf, 512);
}

bool A2_platformDiskWrite(int drive, uint32_t offset, const uint8_t* buf, uint32_t len) {
    DiskImage::Slot& s = DiskImage::slots[drive & 1];
    if (offset + len > s.size || s.readOnly) return false;
    if (s.cache) memcpy(s.cache + offset, buf, len);
    return writeAt(s.file, offset, buf, len);
}
