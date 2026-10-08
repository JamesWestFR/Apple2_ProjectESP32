/*

Apple2_ProjectESP32 — Images de disquette sur la carte SD.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef DiskImage_h
#define DiskImage_h

#include <string>

using namespace std;

#define DISK_EXTENSIONS "dsk,do,po,nib,woz,2mg,hdv"

namespace DiskImage {

// Insère l'image dans le lecteur (0 ou 1). Une image trop grande pour une
// disquette 5,25 pouces (800 Ko à 32 Mo) est montée comme disque dur ProDOS en
// slot 7, quel que soit le lecteur demandé : lastWasHdd le signale. En cas de
// refus, rend faux et la raison dans lastError.
bool insert(int drive, const string& path);
extern bool lastWasHdd;
void ejectHdd();
const string& hddPath();              // "" : pas de disque dur
string hddName();
string baseName(const string& path);  // nom du fichier, sans le dossier
void eject(int drive);
void swap();                          // échange les disquettes des deux lecteurs
const string& path(int drive);        // "" : lecteur vide
string name(int drive);               // nom du fichier, sans le dossier

extern string lastError;

}

#endif // DiskImage_h
