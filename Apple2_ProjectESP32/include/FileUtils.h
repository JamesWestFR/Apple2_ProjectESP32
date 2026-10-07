/*

Apple2_ProjectESP32 — Carte SD : montage et liste de fichiers (adapté d'ESPectrum)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef FileUtils_h
#define FileUtils_h

#include <string>
#include <vector>

using namespace std;

class FileUtils {
public:
    static string MountPoint;   // "/sd"
    static bool SDReady;

    static void initSD();
    static bool mountSDCard(int PIN_MISO, int PIN_MOSI, int PIN_CLK, int PIN_CS);
    static string getLCaseExt(const string& filename);
    // Sous-dossiers (suivis de "/") puis fichiers dont l'extension est dans
    // `exts` (liste séparée par des virgules), triés sans tenir compte de la casse
    // La liste est bornée à LIST_MAX entrées (la mémoire interne est comptée) :
    // listTruncated dit si le dossier en avait davantage.
    static vector<string> listFiles(const string& path, const string& exts);
    static bool listTruncated;
    static const int LIST_MAX = 300;
};

#endif // FileUtils_h
