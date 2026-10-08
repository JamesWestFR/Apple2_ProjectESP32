/*

Apple2_ProjectESP32 — Menu à l'écran et sélecteur de fichiers.

Organisation reprise de CPC_ProjectESP32 : un menu principal qui rappelle ce
qui est inséré et ouvre des sous-menus (supports, machine, manette, vidéo, son,
aide), chacun avec une ou deux lignes d'explication du choix en cours.

Le menu occupe tout l'écran, en 40 colonnes sur 24 lignes, et s'affiche avec
le générateur de caractères de l'Apple : il n'a donc pas de fonte à lui. Les
textes sont en français ou en anglais (T), sans accents, la ROM américaine
n'en ayant pas.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "OSD.h"
#include "Emu.h"
#include "ESPConfig.h"
#include "Video.h"
#include "A2Keyboard.h"
#include "FileUtils.h"
#include "DiskImage.h"
#include "core/A2.h"

using namespace std;
using namespace fabgl;

namespace OSD {

bool active = false;

#define COLS 40
#define ROWS 24
#define TITLE_ROW 2
#define ITEMS_TOP 4
#define LIST_TOP 2
#define LIST_ROWS 19
#define MESSAGE_ROW 22
#define HELP_ROW 23

// Couleurs Apple
#define COL_BG 2        // bleu foncé
#define COL_TEXT 15     // blanc
#define COL_TITLE 13    // jaune
#define COL_INFO 7      // bleu clair

enum { M_MAIN = 0, M_MEDIA, M_MACHINE, M_JOYSTICK, M_VIDEO, M_AUDIO, M_HELP, M_COUNT, M_LIST };

// Ce que choisit la liste de fichiers
enum { TARGET_DRIVE1 = 0, TARGET_DRIVE2, TARGET_HDD, TARGET_RECENT };

static uint8_t screen[ROWS][COLS];     // codes écran Apple
static uint8_t rowColor[ROWS];
static int menu = M_MAIN;
static int sel[M_COUNT];
static string message;
static uint8_t pendingModel = 0;       // modèle choisi, pas encore appliqué

// Liste de fichiers (dossier de la carte SD, ou derniers fichiers)
static int listTarget = TARGET_DRIVE1;
static int listFrom = M_MAIN;          // menu d'où la liste a été ouverte
static vector<string> entries;
static int fileSel = 0, fileTop = 0;

// ---------------------------------------------------------------------------
// Affichage
// ---------------------------------------------------------------------------

// ASCII -> code écran. Normal : $80 + ASCII. Inverse : $00-$3F pour les
// majuscules et les signes, $60-$7F pour les minuscules.
static inline uint8_t screenCode(char ch, bool inverse) {
    uint8_t c = (uint8_t)ch;
    if (c < 0x20 || c > 0x7E) c = '?';
    if (!inverse) return c | 0x80;
    if (c >= 0x40 && c < 0x60) return c - 0x40;
    return c;
}

static void clearScreen() {
    for (int row = 0; row < ROWS; row++) {
        memset(screen[row], screenCode(' ', false), COLS);
        rowColor[row] = COL_TEXT;
    }
}

static void put(int row, int col, const char* text, bool inverse = false) {
    if (row < 0 || row >= ROWS) return;
    for (; *text && col < COLS; text++, col++) screen[row][col] = screenCode(*text, inverse);
}

// Une ligne entière : le texte, complété par des espaces
static void putRow(int row, const char* text, bool inverse = false, uint8_t color = COL_TEXT) {
    if (row < 0 || row >= ROWS) return;
    memset(screen[row], screenCode(' ', inverse), COLS);
    put(row, 0, text, inverse);
    rowColor[row] = color;
}

static void show() {
    for (int row = 0; row < ROWS; row++) Video::drawTextRow(row, screen[row], rowColor[row], COL_BG);
}

// Fin d'un texte trop long pour la largeur donnée, précédée de ".."
static string tail(const string& s, size_t width) {
    if (s.size() <= width) return s;
    return ".." + s.substr(s.size() - (width - 2));
}

static string head(const string& s, size_t width) {
    return s.size() <= width ? s : s.substr(0, width);
}

// Bandeau du haut, titre du menu et ligne des touches
static void frame(const char* title, const char* keys) {
    clearScreen();
    putRow(0, " APPLE2_PROJECTESP32", true);
    string m = A2::modelName(A2::model());
    put(0, COLS - 1 - (int)m.size(), m.c_str(), true);
    if (title) putRow(TITLE_ROW, (string(" ") + title).c_str(), false, COL_TITLE);
    putRow(HELP_ROW, keys, true);
}

static const char* yesNo(bool v) { return v ? T("oui", "yes") : T("non", "no"); }

// ---------------------------------------------------------------------------
// Contenu des menus
// ---------------------------------------------------------------------------

enum { MAIN_MEDIA = 0, MAIN_RECENT, MAIN_SAVESTATE, MAIN_LOADSTATE, MAIN_MACHINE, MAIN_JOYSTICK, MAIN_VIDEO, MAIN_AUDIO, MAIN_WIFI, MAIN_HELP,
       MAIN_RESET, MAIN_LANGUAGE, MAIN_CLOSE, MAIN_COUNT };
enum { MEDIA_DRIVE1 = 0, MEDIA_DRIVE2, MEDIA_HDD, MEDIA_EJECT1, MEDIA_EJECT2, MEDIA_EJECTHDD, MEDIA_SWAP,
       MEDIA_BACK, MEDIA_COUNT };
enum { MAC_MODEL = 0, MAC_APPLY, MAC_KEYB, MAC_FASTDISK, MAC_INFO, MAC_PERGAME, MAC_MOUSE, MAC_COLDBOOT, MAC_RESTART, MAC_BACK, MAC_COUNT };
enum { JOY_MODE = 0, JOY_BACK, JOY_COUNT };
enum { VID_MONITOR = 0, VID_DHGR, VID_CHARSET, VID_SCANLINES, VID_BACK, VID_COUNT };
enum { AUD_VOLUME = 0, AUD_MOCKINGBOARD, AUD_BACK, AUD_COUNT };

static int itemCount(int m) {
    static const int counts[M_COUNT] = { MAIN_COUNT, MEDIA_COUNT, MAC_COUNT, JOY_COUNT, VID_COUNT, AUD_COUNT, 0 };
    return counts[m];
}

static const char* menuTitle(int m) {
    switch (m) {
        case M_MEDIA: return T("Disquettes et disque dur", "Disks and hard disk");
        case M_MACHINE: return T("Reglages de la machine", "Machine settings");
        case M_JOYSTICK: return T("Reglages de la manette", "Joystick settings");
        case M_VIDEO: return T("Reglages video", "Video settings");
        case M_AUDIO: return T("Reglages du son", "Audio settings");
        case M_HELP: return T("Aide : les touches", "Help: keys");
        default: return nullptr;
    }
}

static string driveLabel(int d) {
    return DiskImage::path(d).empty() ? T("(vide)", "(empty)") : DiskImage::name(d);
}

static string hddLabel() {
    return DiskImage::hddPath().empty() ? T("(aucun)", "(none)") : DiskImage::hddName();
}

static const char* monitorName(uint8_t m) {
    switch (m) {
        case MONITOR_WHITE: return T("blanc", "white");
        case MONITOR_GREEN: return T("vert", "green");
        case MONITOR_AMBER: return T("ambre", "amber");
        default: return T("couleur", "colour");
    }
}

static string itemText(int m, int i) {
    char buf[64];
    switch (m) {
        case M_MAIN:
            switch (i) {
                case MAIN_MEDIA: return T("Disquettes et disque dur...", "Disks and hard disk...");
                case MAIN_RECENT: return T("Derniers fichiers...", "Recent files...");
                case MAIN_SAVESTATE: return T("Sauver l'etat du jeu (Ctrl+F9)", "Save game state (Ctrl+F9)");
                case MAIN_LOADSTATE: return T("Reprendre l'etat sauve (Ctrl+F10)", "Load saved state (Ctrl+F10)");
                case MAIN_MACHINE: return T("Reglages de la machine...", "Machine settings...");
                case MAIN_JOYSTICK: return T("Reglages de la manette...", "Joystick settings...");
                case MAIN_VIDEO: return T("Reglages video...", "Video settings...");
                case MAIN_AUDIO: return T("Reglages du son...", "Audio settings...");
                case MAIN_WIFI: return T("Transfert de fichiers WiFi...", "WiFi file transfer...");
                case MAIN_HELP: return T("Aide : les touches...", "Help: keys...");
                case MAIN_RESET: return T("Reset de l'Apple (Ctrl-Reset)", "Reset Apple (Ctrl-Reset)");
                case MAIN_LANGUAGE: return Config::language ? "Langues/Languages : EN" : "Langues/Languages : FR";
                default: return T("Fermer", "Close");
            }
        case M_MEDIA:
            switch (i) {
                case MEDIA_DRIVE1: return T("Inserer une disquette : lecteur 1", "Insert disk in drive 1");
                case MEDIA_DRIVE2: return T("Inserer une disquette : lecteur 2", "Insert disk in drive 2");
                case MEDIA_HDD: return T("Monter un disque dur", "Mount hard disk");
                case MEDIA_EJECT1:
                    return DiskImage::path(0).empty() ? T("Ejecter le lecteur 1 (vide)", "Eject drive 1 (empty)")
                                                      : T("Ejecter le lecteur 1", "Eject drive 1");
                case MEDIA_EJECT2:
                    return DiskImage::path(1).empty() ? T("Ejecter le lecteur 2 (vide)", "Eject drive 2 (empty)")
                                                      : T("Ejecter le lecteur 2", "Eject drive 2");
                case MEDIA_EJECTHDD:
                    return DiskImage::hddPath().empty() ? T("Retirer le disque dur (aucun)", "Remove hard disk (none)")
                                                        : T("Retirer le disque dur", "Remove hard disk");
                case MEDIA_SWAP: return T("Echanger les disquettes 1 et 2", "Swap disks 1 and 2");
                default: return T("Retour", "Back");
            }
        case M_MACHINE:
            switch (i) {
                case MAC_MODEL:
                    snprintf(buf, sizeof(buf), T("Modele : %s", "Model: %s"), A2::modelName((A2::Model)pendingModel));
                    return buf;
                case MAC_APPLY:
                    return pendingModel != A2::model() ? T("Appliquer le modele (redemarre l'Apple)", "Apply model (restarts the Apple)")
                                                       : T("Appliquer le modele (inchange)", "Apply model (no change)");
                case MAC_KEYB:
                    snprintf(buf, sizeof(buf), T("Clavier du PC : %s", "PC keyboard: %s"),
                             Config::keyLayout == KEYB_FR ? "AZERTY" : "QWERTY");
                    return buf;
                case MAC_FASTDISK:
                    snprintf(buf, sizeof(buf), T("Disquette rapide : %s", "Fast disk: %s"), yesNo(Config::fastDisk));
                    return buf;
                case MAC_INFO:
                    snprintf(buf, sizeof(buf), T("Infos a l'ecran : %s", "On-screen info: %s"),
                             Config::screenInfo == 2 ? T("lecteurs + vitesse", "drives + speed")
                             : Config::screenInfo ? T("lecteurs", "drives") : T("aucune", "none"));
                    return buf;
                case MAC_PERGAME:
                    snprintf(buf, sizeof(buf), T("Reglages par jeu : %s", "Per-game settings: %s"), yesNo(Config::perGame));
                    return buf;
                case MAC_MOUSE:
                    snprintf(buf, sizeof(buf), T("Carte souris (slot 2) : %s", "Mouse card (slot 2): %s"), yesNo(Config::mouse));
                    return buf;
                case MAC_COLDBOOT: return T("Redemarrer l'Apple a froid", "Cold boot the Apple");
                case MAC_RESTART: return T("Redemarrer l'ESP32", "Restart ESP32");
                default: return T("Retour", "Back");
            }
        case M_JOYSTICK:
            if (i == JOY_MODE) {
                snprintf(buf, sizeof(buf), T("Manette au clavier : %s", "Keyboard joystick: %s"),
                         Config::joystick ? T("fleches", "arrows") : T("non", "off"));
                return buf;
            }
            return T("Retour", "Back");
        case M_VIDEO:
            switch (i) {
                case VID_MONITOR:
                    snprintf(buf, sizeof(buf), T("Moniteur : %s", "Monitor: %s"), monitorName(Config::monitor));
                    return buf;
                case VID_DHGR:
                    snprintf(buf, sizeof(buf), T("Double haute res. : %s", "Double hi-res: %s"),
                             Config::dhgrMono ? T("monochrome", "monochrome") : T("couleur", "colour"));
                    return buf;
                case VID_CHARSET:
                    snprintf(buf, sizeof(buf), T("Caracteres : %s", "Characters: %s"),
                             Config::charset ? T("francais", "French") : T("americains", "US"));
                    return buf;
                case VID_SCANLINES:
                    snprintf(buf, sizeof(buf), T("Lignes de balayage : %s", "Scanlines: %s"), yesNo(Config::scanlines));
                    return buf;
                default: return T("Retour", "Back");
            }
        case M_AUDIO:
            switch (i) {
                case AUD_VOLUME:
                    snprintf(buf, sizeof(buf), "Volume : %d  %s", Config::volume, T("(gauche / droite)", "(left / right)"));
                    return buf;
                case AUD_MOCKINGBOARD:
                    snprintf(buf, sizeof(buf), "Mockingboard (slot 4) : %s", yesNo(Config::mockingboard));
                    return buf;
                default: return T("Retour", "Back");
            }
        default: return "";
    }
}

// Explication du choix en cours, sur deux lignes au plus
static void itemHelp(int m, int i, const char*& a, const char*& b) {
    a = b = nullptr;
    switch (m) {
        case M_MEDIA:
            if (i == MEDIA_DRIVE1) {
                a = T("Images DSK, DO, PO, NIB, WOZ, 2MG.", "DSK, DO, PO, NIB, WOZ, 2MG images.");
                b = T("Entree : demarrer. Espace : inserer.", "Return: boots it. Space: inserts only.");
            } else if (i == MEDIA_DRIVE2) {
                a = T("Seconde disquette d'un programme.", "Second disk of a program.");
            } else if (i == MEDIA_HDD) {
                a = T("Images HDV, PO, 2MG : slot 7 du //e,", "HDV, PO, 2MG images: //e slot 7,");
                b = T("SmartPort du //c, 3,5 pouces du //c+.", "//c SmartPort, //c+ 3.5 inch drive.");
            } else if (i == MEDIA_EJECTHDD) {
                a = T("L'Apple redemarre sur la disquette.", "The Apple boots from the floppy again.");
            } else if (i == MEDIA_SWAP) {
                a = T("Pour un jeu sur deux faces qui ne", "For a two-sided game that only");
                b = T("connait que le lecteur 1.", "knows drive 1.");
            }
            break;
        case M_MACHINE:
            if (i == MAC_MODEL || i == MAC_APPLY) {
                a = T("][ : BASIC entier. ][+ : Applesoft.", "][: Integer BASIC. ][+: Applesoft.");
                b = T("//e : cartes. //c : ROM 255 a 4, Plus.", "//e: cards. //c: ROM 255 to 4, Plus.");
            } else if (i == MAC_KEYB) {
                a = T("La touche marquee A donne un A.", "The key labelled A types an A.");
            } else if (i == MAC_FASTDISK) {
                a = T("Emulation acceleree tant que le lecteur", "Faster emulation while the drive");
                b = T("tourne. Le son est alors hache.", "spins. Sound is choppy meanwhile.");
            } else if (i == MAC_INFO) {
                a = T("Lecteurs : 1, 2 ou H quand ils lisent.", "Drives: 1, 2 or H while they work.");
                b = T("Vitesse : images par seconde, charge.", "Speed: frames per second, load.");
            } else if (i == MAC_PERGAME) {
                a = T("Modele, manette, moniteur memorises", "Model, joystick, monitor kept for");
                b = T("pour chaque image, au demarrage du jeu.", "each image, applied when it boots.");
            } else if (i == MAC_MOUSE) {
                a = T("Souris PS/2 sur la seconde prise :", "PS/2 mouse on the second socket:");
                b = T("prise en compte en redemarrant l'ESP32.", "used after restarting the ESP32.");
            } else if (i == MAC_COLDBOOT) {
                a = T("Comme eteindre et rallumer l'Apple.", "Like switching the Apple off and on.");
            }
            break;
        case M_JOYSTICK:
            a = T("Fleches : directions (plus de curseur).", "Arrows: directions (no cursor keys).");
            b = T("Ctrl droite, Maj droite : boutons 0, 1.", "Right Ctrl, right Shift: buttons 0, 1.");
            break;
        case M_VIDEO:
            if (i == VID_MONITOR) {
                a = T("Blanc, vert, ambre : sans couleurs,", "White, green, amber: no colours,");
                b = T("plus net en texte et en 80 colonnes.", "sharper for text and 80 columns.");
            } else if (i == VID_DHGR) {
                a = T("Monochrome : texte fin lisible dans les", "Monochrome: fine text is readable in");
                b = T("programmes de bureau (A2DeskTop).", "desktop programs (A2DeskTop).");
            } else if (i == VID_CHARSET) {
                a = T("Francais (//e, //c) : des accents a la", "French (//e, //c): accents instead of");
                b = T("place de @ [ ] { } # et au clavier.", "@ [ ] { } etc. Keyboard types them.");
            } else if (i == VID_SCANLINES) {
                a = T("Une ligne de l'ecran sur deux en noir.", "Every other screen line is black.");
            }
            break;
        case M_AUDIO:
            if (i == AUD_VOLUME) {
                a = T("4 : niveau normal. 8 : le double.", "4: normal level. 8: twice as loud.");
            } else if (i == AUD_MOCKINGBOARD) {
                a = T("Carte son a six voies, pour les", "Six-voice sound card, for the");
                b = T("programmes qui la reconnaissent.", "programs that detect it.");
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Dessin des menus
// ---------------------------------------------------------------------------

static void drawHelpPage() {
    frame(menuTitle(M_HELP), T(" Echap : retour", " Esc: back"));
    static const char* const fr[] = {
        "F12        menu",
        "F11        choix d'une disquette",
        "Ctrl+F11   Ctrl-Reset",
        "Ctrl+F12   redemarrage a froid",
        "F9         manette sur les fleches",
        "Ctrl+F9    sauver l'etat du jeu",
        "Ctrl+F10   reprendre l'etat sauve",
        "Alt gauche Pomme ouverte (bouton 0)",
        "Windows    Pomme pleine (bouton 1)",
        "Verr Maj   allume : minuscules",
        "Suppr      touche DELETE du //e",
        "Pause      fige l'emulation",
        "Arret def. vitesse maximale",
        "Impr ecran capture sur la carte SD",
        "Liste de fichiers :",
        "Entree     inserer et demarrer",
        "Espace     inserer sans redemarrer",
        "Ret. arr.  dossier parent",
        "une lettre fichier qui commence ainsi",
    };
    static const char* const en[] = {
        "F12        menu",
        "F11        choose a disk",
        "Ctrl+F11   Ctrl-Reset",
        "Ctrl+F12   cold boot",
        "F9         joystick on the arrows",
        "Ctrl+F9    save the game state",
        "Ctrl+F10   load the saved state",
        "Left Alt   Open Apple (button 0)",
        "Windows    Solid Apple (button 1)",
        "Caps Lock  on: lower case allowed",
        "Del        DELETE key of the //e",
        "Pause      freezes the emulation",
        "Scroll Lk  full speed",
        "Print Scr  screenshot to the SD card",
        "File list:",
        "Return     insert and boot",
        "Space      insert without booting",
        "Backspace  parent folder",
        "a letter   next file starting with it",
    };
    const char* const* lines = Config::language ? en : fr;
    for (int i = 0; i < 19; i++) put(ITEMS_TOP - 1 + i, 1, lines[i]);
    show();
}

static void drawMenu() {
    if (menu == M_HELP) { drawHelpPage(); return; }
    bool isMain = menu == M_MAIN;
    frame(menuTitle(menu), isMain ? T(" Haut/Bas  Entree:OK  F12/Echap:fermer", " Up/Down  Return: OK  F12/Esc: close")
                                  : T(" Haut/Bas  Entree:changer  Echap:retour", " Up/Down  Return: change  Esc: back"));
    int row = isMain ? 2 : ITEMS_TOP;

    // Sous-menu des supports : rappel de ce qui est inséré
    if (menu == M_MEDIA) {
        char line[64];
        for (int d = 0; d < 2; d++) {
            snprintf(line, sizeof(line), T("   Lecteur %d : %s", "   Drive %d: %s"), d + 1, head(driveLabel(d), 24).c_str());
            putRow(row++, line, false, COL_INFO);
        }
        snprintf(line, sizeof(line), T("   Disque dur : %s", "   Hard disk: %s"), head(hddLabel(), 24).c_str());
        putRow(row++, line, false, COL_INFO);
        row++;
    }

    int count = itemCount(menu);
    for (int i = 0; i < count; i++) {
        string text = " " + itemText(menu, i);
        if (i == sel[menu]) putRow(row, text.c_str(), true);
        else put(row, 0, text.c_str());
        row++;
        // Menu principal : sous le choix des supports, ce qui est inséré
        if (isMain && i == MAIN_MEDIA) {
            char line[64];
            for (int d = 0; d < 2; d++) {
                if (d == 1 && DiskImage::path(1).empty()) continue;
                snprintf(line, sizeof(line), "    %d: %s", d + 1, head(driveLabel(d), 30).c_str());
                putRow(row++, line, false, COL_INFO);
            }
            if (!DiskImage::hddPath().empty()) {
                snprintf(line, sizeof(line), T("    DD: %s", "    HD: %s"), head(hddLabel(), 30).c_str());
                putRow(row++, line, false, COL_INFO);
            }
        }
    }

    // Explication du choix en cours
    const char *a, *b;
    itemHelp(menu, sel[menu], a, b);
    row++;
    if (a) putRow(row++, (string(" ") + a).c_str(), false, COL_INFO);
    if (b) putRow(row++, (string(" ") + b).c_str(), false, COL_INFO);
    if (!message.empty()) putRow(MESSAGE_ROW, (" " + head(message, COLS - 2)).c_str(), false, COL_TITLE);
    show();
}

// ---------------------------------------------------------------------------
// Liste de fichiers
// ---------------------------------------------------------------------------

static void drawList() {
    bool recent = listTarget == TARGET_RECENT;
    const char* keys = (listTarget == TARGET_DRIVE2) ? T(" Entree:inserer  Echap:retour", " Return: insert  Esc: back")
                                                     : T(" Entree:demarrer  Espace:inserer  Echap", " Return: boot  Space: insert  Esc");
    frame(nullptr, keys);
    char title[64];
    if (recent) snprintf(title, sizeof(title), "%s", T(" DERNIERS FICHIERS", " RECENT FILES"));
    else if (listTarget == TARGET_HDD) snprintf(title, sizeof(title), T(" DISQUE DUR : %s", " HARD DISK: %s"), tail(Config::lastDir, 24).c_str());
    else snprintf(title, sizeof(title), T(" LECTEUR %d : %s", " DRIVE %d: %s"), listTarget + 1, tail(Config::lastDir, 26).c_str());
    putRow(0, title, true);
    if (entries.empty()) {
        put(LIST_TOP + 1, 1, recent ? T("Aucun fichier utilise pour l'instant", "No file used yet")
                            : FileUtils::SDReady ? T("Aucune image dans ce dossier", "No image in this folder")
                                                 : T("Carte SD absente", "No SD card"));
    }
    for (int i = 0; i < LIST_ROWS && fileTop + i < (int)entries.size(); i++) {
        const string& entry = entries[fileTop + i];
        string text = " " + head(recent ? DiskImage::baseName(entry) : entry, COLS - 2);
        if (fileTop + i == fileSel) putRow(LIST_TOP + i, text.c_str(), true);
        else put(LIST_TOP + i, 0, text.c_str());
    }
    // Nom trop long pour sa ligne : sa suite, sous la liste (les faces A et B
    // d'un jeu ne diffèrent souvent que par la fin du nom)
    if (!entries.empty()) {
        string name = recent ? DiskImage::baseName(entries[fileSel]) : entries[fileSel];
        if (name.size() > COLS - 2) {
            putRow(MESSAGE_ROW - 1, (" " + head(name.substr(COLS - 2), COLS - 2)).c_str(), false, COL_INFO);
            if (name.size() > 2 * (COLS - 2))
                putRow(MESSAGE_ROW, (" " + head(name.substr(2 * (COLS - 2)), COLS - 2)).c_str(), false, COL_INFO);
        }
    }
    if (!message.empty()) putRow(MESSAGE_ROW, (" " + head(message, COLS - 2)).c_str(), false, COL_TITLE);
    show();
}

static void loadDir() {
    entries.clear();
    if (FileUtils::SDReady) {
        entries = FileUtils::listFiles(Config::lastDir, DISK_EXTENSIONS);
        if (Config::lastDir != FileUtils::MountPoint) entries.insert(entries.begin(), "../");
        if (FileUtils::listTruncated) message = T("Dossier trop grand : 300 premiers", "Folder too big: first 300 shown");
    }
    fileSel = fileTop = 0;
}

static void openList(int target) {
    listTarget = target;
    listFrom = menu == M_LIST ? M_MAIN : menu;
    menu = M_LIST;
    message = "";
    if (target == TARGET_RECENT) {
        entries = Config::recent;
        fileSel = fileTop = 0;
    } else {
        loadDir();
        // Dossier mémorisé disparu : retour à la racine de la carte
        if (entries.empty() && Config::lastDir != FileUtils::MountPoint) {
            Config::lastDir = FileUtils::MountPoint;
            loadDir();
        }
    }
    drawList();
}

static void moveFileSel(int to) {
    int count = (int)entries.size();
    if (count == 0) return;
    if (to < 0) to = 0;
    if (to >= count) to = count - 1;
    fileSel = to;
    if (fileSel < fileTop) fileTop = fileSel;
    if (fileSel >= fileTop + LIST_ROWS) fileTop = fileSel - LIST_ROWS + 1;
    drawList();
}

static void close();

static void parentDir() {
    if (listTarget == TARGET_RECENT || Config::lastDir == FileUtils::MountPoint) return;
    size_t slash = Config::lastDir.find_last_of('/');
    string left = Config::lastDir.substr(slash + 1) + "/";
    Config::lastDir = Config::lastDir.substr(0, slash);
    Config::dirty = true;
    loadDir();
    // Revient sur le dossier qu'on quitte
    for (int i = 0; i < (int)entries.size(); i++)
        if (entries[i] == left) { moveFileSel(i); return; }
    drawList();
}

static void chooseFile(bool boot) {
    if (entries.empty()) return;
    string entry = entries[fileSel];
    string path;
    if (listTarget == TARGET_RECENT) {
        path = entry;
    } else {
        if (entry == "../") { parentDir(); return; }
        if (entry.back() == '/') {
            Config::lastDir += "/" + entry.substr(0, entry.size() - 1);
            Config::dirty = true;
            loadDir();
            drawList();
            return;
        }
        path = Config::lastDir + "/" + entry;
    }
    int drive = listTarget == TARGET_DRIVE2 ? 1 : 0;
    if (!DiskImage::insert(drive, path)) {
        message = DiskImage::lastError;
        drawList();
        return;
    }
    // Une grande image est allée dans le disque dur, pas dans le lecteur
    if (DiskImage::lastWasHdd) Config::hdd = path;
    else Config::disk[drive] = path;
    Config::addRecent(path);
    Config::dirty = true;
    if ((boot && drive == 0) || DiskImage::lastWasHdd) Emu::coldBootRequest = true;
    close();
}

static void leaveList() {
    entries.clear();
    entries.shrink_to_fit();
    menu = listFrom;
    message = "";
    drawMenu();
}

static void keyList(VirtualKey vk, uint8_t ascii) {
    message = "";
    switch (vk) {
        case VK_UP: case VK_KP_UP: moveFileSel(fileSel - 1); return;
        case VK_DOWN: case VK_KP_DOWN: moveFileSel(fileSel + 1); return;
        case VK_PAGEUP: case VK_KP_PAGEUP: moveFileSel(fileSel - LIST_ROWS); return;
        case VK_PAGEDOWN: case VK_KP_PAGEDOWN: moveFileSel(fileSel + LIST_ROWS); return;
        case VK_HOME: case VK_KP_HOME: moveFileSel(0); return;
        case VK_END: case VK_KP_END: moveFileSel((int)entries.size() - 1); return;
        case VK_RETURN: case VK_KP_ENTER: chooseFile(true); return;
        case VK_BACKSPACE: case VK_LEFT: case VK_KP_LEFT:
            if (listTarget == TARGET_RECENT || Config::lastDir == FileUtils::MountPoint) leaveList();
            else parentDir();
            return;
        case VK_ESCAPE: leaveList(); return;
        default: break;
    }
    if (ascii == ' ') { chooseFile(false); return; }
    // Une lettre ou un chiffre : entrée suivante qui commence ainsi
    if (ascii > ' ' && ascii < 0x7F && !entries.empty()) {
        int count = (int)entries.size();
        for (int i = 1; i <= count; i++) {
            int idx = (fileSel + i) % count;
            string name = listTarget == TARGET_RECENT ? DiskImage::baseName(entries[idx]) : entries[idx];
            if (!name.empty() && toupper((unsigned char)name[0]) == toupper(ascii)) { moveFileSel(idx); return; }
        }
    }
}

// ---------------------------------------------------------------------------
// Touches des menus
// ---------------------------------------------------------------------------

static void enter(int m) {
    menu = m;
    message = "";
    if (m == M_MACHINE) pendingModel = Config::model;
    drawMenu();
}

// Entrée, ou flèche gauche / droite (`dir` : -1, 0 pour Entrée, +1)
static void activate(int m, int i, int dir) {
    int step = dir ? dir : 1;
    const bool ok = dir == 0;
    switch (m) {
        case M_MAIN:
            if (!ok && i != MAIN_LANGUAGE) return;
            switch (i) {
                case MAIN_MEDIA: enter(M_MEDIA); return;
                case MAIN_RECENT: openList(TARGET_RECENT); return;
                case MAIN_SAVESTATE: case MAIN_LOADSTATE:
                    close();
                    Emu::stateRequest = i == MAIN_SAVESTATE ? 1 : 2;
                    return;
                case MAIN_MACHINE: enter(M_MACHINE); return;
                case MAIN_JOYSTICK: enter(M_JOYSTICK); return;
                case MAIN_VIDEO: enter(M_VIDEO); return;
                case MAIN_AUDIO: enter(M_AUDIO); return;
                case MAIN_WIFI: {
                    // Le prochain démarrage se fait sur le firmware WiFi, qui rend
                    // lui-même la main à l'émulateur pour le suivant
                    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
                    esp_app_desc_t desc;
                    if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) {
                        message = T("Absent : build.ps1 -Env wifi -Flash", "Missing: build.ps1 -Env wifi -Flash");
                        break;
                    }
                    A2::Disk::flush();
                    Config::saveIfDirty();
                    if (esp_ota_set_boot_partition(part) == ESP_OK) esp_restart();
                    return;
                }
                case MAIN_HELP: enter(M_HELP); return;
                case MAIN_RESET: Emu::resetRequest = true; close(); return;
                case MAIN_LANGUAGE: Config::language = !Config::language; Config::dirty = true; break;
                default: close(); return;
            }
            break;
        case M_MEDIA:
            if (!ok) return;
            switch (i) {
                case MEDIA_DRIVE1: openList(TARGET_DRIVE1); return;
                case MEDIA_DRIVE2: openList(TARGET_DRIVE2); return;
                case MEDIA_HDD: openList(TARGET_HDD); return;
                case MEDIA_EJECT1: case MEDIA_EJECT2:
                    DiskImage::eject(i - MEDIA_EJECT1);
                    Config::disk[i - MEDIA_EJECT1] = "";
                    Config::dirty = true;
                    break;
                case MEDIA_EJECTHDD:
                    DiskImage::ejectHdd();
                    Config::hdd = "";
                    Config::dirty = true;
                    break;
                case MEDIA_SWAP:
                    DiskImage::swap();
                    Config::disk[0] = DiskImage::path(0);
                    Config::disk[1] = DiskImage::path(1);
                    Config::dirty = true;
                    break;
                default: enter(M_MAIN); return;
            }
            break;
        case M_MACHINE:
            switch (i) {
                case MAC_MODEL:
                    pendingModel = (uint8_t)((pendingModel + A2::MODEL_COUNT + step) % A2::MODEL_COUNT);
                    break;
                case MAC_APPLY:
                    if (!ok || pendingModel == A2::model()) return;
                    Config::model = pendingModel;
                    Config::dirty = true;
                    close();        // le changement de modèle redémarre l'Apple
                    return;
                case MAC_KEYB:
                    Config::keyLayout = (uint8_t)((Config::keyLayout + KEYB_COUNT + step) % KEYB_COUNT);
                    Config::dirty = true;
                    break;
                case MAC_FASTDISK:
                    Config::fastDisk = !Config::fastDisk;
                    Config::dirty = true;
                    break;
                case MAC_INFO:
                    Config::screenInfo = (uint8_t)((Config::screenInfo + 3 + step) % 3);
                    Config::dirty = true;
                    break;
                case MAC_PERGAME:
                    Config::perGame = !Config::perGame;
                    // Les réglages du jeu en place entrent en vigueur, ou les
                    // réglages généraux reviennent
                    Config::switchGame(Emu::gameName());
                    pendingModel = Config::model;
                    Video::setMonitor(Config::monitor);
                    Config::dirty = true;
                    break;
                case MAC_MOUSE:
                    Config::mouse = !Config::mouse;
                    Config::dirty = true;
                    A2::Mouse::setEnabled(Config::mouse);
                    break;
                case MAC_COLDBOOT:
                    if (!ok) return;
                    Emu::coldBootRequest = true;
                    close();
                    return;
                case MAC_RESTART:
                    if (!ok) return;
                    A2::Disk::flush();
                    Config::saveIfDirty();
                    esp_restart();
                    return;
                default:
                    if (ok) enter(M_MAIN);
                    return;
            }
            break;
        case M_JOYSTICK:
            if (i == JOY_MODE) {
                Config::joystick = !Config::joystick;
                Config::dirty = true;
            } else {
                if (ok) enter(M_MAIN);
                return;
            }
            break;
        case M_VIDEO:
            switch (i) {
                case VID_MONITOR:
                    Config::monitor = (uint8_t)((Config::monitor + MONITOR_COUNT + step) % MONITOR_COUNT);
                    Config::dirty = true;
                    Video::setMonitor(Config::monitor);
                    break;
                case VID_DHGR:
                    Config::dhgrMono = !Config::dhgrMono;
                    Config::dirty = true;
                    Video::setMonitor(Config::monitor);
                    break;
                case VID_CHARSET:
                    Config::charset = !Config::charset;
                    Config::dirty = true;
                    Video::setMonitor(Config::monitor);
                    break;
                case VID_SCANLINES:
                    Config::scanlines = !Config::scanlines;
                    Config::dirty = true;
                    Video::setScanlines(Config::scanlines);
                    break;
                default:
                    if (ok) enter(M_MAIN);
                    return;
            }
            break;
        case M_AUDIO:
            switch (i) {
                case AUD_VOLUME: {
                    int v = Config::volume + (dir ? dir : 1);
                    if (v > 8) v = dir ? 8 : 0;
                    if (v < 0) v = 0;
                    Config::volume = (uint8_t)v;
                    Config::dirty = true;
                    break;
                }
                case AUD_MOCKINGBOARD:
                    Config::mockingboard = !Config::mockingboard;
                    Config::dirty = true;
                    A2::Mockingboard::setEnabled(Config::mockingboard);
                    break;
                default:
                    if (ok) enter(M_MAIN);
                    return;
            }
            break;
        default: return;
    }
    drawMenu();
}

static void keyMenu(VirtualKey vk) {
    message = "";
    if (menu == M_HELP) {
        if (vk == VK_ESCAPE || vk == VK_BACKSPACE || vk == VK_RETURN || vk == VK_KP_ENTER) enter(M_MAIN);
        return;
    }
    int count = itemCount(menu);
    int& s = sel[menu];
    switch (vk) {
        case VK_UP: case VK_KP_UP: s = (s + count - 1) % count; drawMenu(); return;
        case VK_DOWN: case VK_KP_DOWN: s = (s + 1) % count; drawMenu(); return;
        case VK_HOME: case VK_KP_HOME: s = 0; drawMenu(); return;
        case VK_END: case VK_KP_END: s = count - 1; drawMenu(); return;
        case VK_LEFT: case VK_KP_LEFT: activate(menu, s, -1); return;
        case VK_RIGHT: case VK_KP_RIGHT: activate(menu, s, 1); return;
        case VK_RETURN: case VK_KP_ENTER: activate(menu, s, 0); return;
        case VK_ESCAPE: case VK_BACKSPACE:
            if (menu == M_MAIN) close();
            else enter(M_MAIN);
            return;
        default: return;
    }
}

void key(VirtualKey vk, uint8_t ascii) {
    if (!active) return;
    if (menu == M_LIST) keyList(vk, ascii);
    else keyMenu(vk);
}

static void close() {
    active = false;
    entries.clear();
    entries.shrink_to_fit();
    message = "";
    Config::saveIfDirty();
    // Changement de machine : l'Apple redémarre à froid avec ses nouvelles ROM
    if (Config::model != A2::model()) {
        A2::Disk::flush();
        A2::setModel((A2::Model)Config::model);
    }
    Keyb::releaseAll();
}

void toggle(int which) {
    if (active) {
        close();
        return;
    }
    active = true;
    Keyb::releaseAll();
    menu = M_MAIN;
    if (which == MENU_FILES) openList(TARGET_DRIVE1);
    else drawMenu();
}

}
