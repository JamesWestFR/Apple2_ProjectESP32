/*

Apple2_ProjectESP32, an Apple II emulator for Espressif ESP32 SoC

Réglages, mémorisés dans la flash de l'ESP32 (NVS).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef ESPConfig_h
#define ESPConfig_h

#include <stdint.h>
#include <string>
#include <vector>

using namespace std;

enum { KEYB_FR = 0, KEYB_US, KEYB_COUNT };
enum { MONITOR_COLOR = 0, MONITOR_WHITE, MONITOR_GREEN, MONITOR_AMBER, MONITOR_COUNT };

class Config {
public:
    static uint8_t esp32rev;     // révision de la puce : choisit les paramètres de l'horloge vidéo (I2S.cpp)

    static uint8_t model;        // A2::Model
    static uint8_t keyLayout;    // KEYB_FR : clavier PC AZERTY, KEYB_US : QWERTY
    static uint8_t joystick;     // manette sur les flèches
    static uint8_t monitor;      // MONITOR_*
    static uint8_t scanlines;    // une ligne VGA sur deux en noir
    static uint8_t dhgrMono;     // double haute résolution sans couleurs
    static uint8_t fringes;      // haute résolution : points isolés en couleur, comme sur un vrai moniteur
    static uint8_t charset;      // caractères du //e et du //c : 0 américains, 1 français
    static uint8_t fastDisk;     // émulation accélérée tant que le lecteur tourne
    static uint8_t volume;       // 0 à 8
    static uint8_t mockingboard; // carte son dans le slot 4
    static uint8_t mouse;        // carte souris dans le slot 2, souris PS/2 sur la seconde prise
    static uint8_t language;     // 0 : français, 1 : anglais
    static uint8_t screenInfo;   // 0 : rien, 1 : voyants des lecteurs, 2 : voyants et vitesse
    static string lastDir;       // dernier dossier ouvert dans le sélecteur de fichiers
    static string disk[2];       // disquettes restées dans les lecteurs
    static string hdd;           // image du disque dur
    static vector<string> recent; // derniers fichiers utilisés, le plus récent d'abord

    // Réglages par jeu : le modèle, la manette, le moniteur et la double haute
    // résolution sont mémorisés pour chaque image, dans A2GAMES.CFG sur la carte SD.
    // Un jeu sans réglages reçoit les réglages généraux.
    static uint8_t perGame;
    static string currentGame;   // jeu dont les réglages sont en vigueur ("" : réglages généraux)
    static void switchGame(const string& name);
    static void addRecent(const string& path);

    static bool dirty;

    static void load();
    static void save();
    static void saveIfDirty();
};

// Texte du menu dans la langue choisie
#define T(fr, en) (Config::language ? (en) : (fr))

#endif // ESPConfig_h
