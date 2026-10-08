/*

Apple2_ProjectESP32 — Réglages mémorisés en NVS.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <string.h>
#include "ESPConfig.h"
#include "core/A2.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "apple2"

uint8_t Config::esp32rev = 0;
uint8_t Config::model = A2::MODEL_IIE_ENH;
uint8_t Config::keyLayout = KEYB_FR;
uint8_t Config::joystick = 0;
uint8_t Config::monitor = MONITOR_COLOR;
uint8_t Config::scanlines = 0;
uint8_t Config::dhgrMono = 0;
uint8_t Config::fringes = 0;
uint8_t Config::charset = 0;
uint8_t Config::fastDisk = 1;
uint8_t Config::volume = 4;
uint8_t Config::mockingboard = 1;
uint8_t Config::mouse = 0;
uint8_t Config::language = 0;
uint8_t Config::screenInfo = 1;
vector<string> Config::recent;
uint8_t Config::perGame = 0;
string Config::currentGame = "";

// Réglages généraux, ceux de la flash : les valeurs en vigueur (model, joystick,
// monitor, dhgrMono) peuvent être celles d'un jeu
static uint8_t gModel, gJoystick, gMonitor, gDhgrMono;

#define GAMES_FILE "/sd/A2GAMES.CFG"
#define GAMES_LINE 160

// Une ligne par jeu : NOM|modèle manette moniteur dhgr
static bool readProfile(const string& name, int v[4]) {
    FILE* f = fopen(GAMES_FILE, "r");
    if (!f) return false;
    char line[GAMES_LINE];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char* bar = strrchr(line, '|');
        if (!bar || name.compare(0, string::npos, line, bar - line) != 0) continue;
        found = sscanf(bar + 1, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4;
    }
    fclose(f);
    return found;
}

static void writeProfile(const string& name, const int v[4]) {
    // Rien à écrire si rien n'a changé, ni pour un jeu sans réglages propres
    // tant qu'il garde les réglages généraux
    int old[4];
    const int general[4] = { gModel, gJoystick, gMonitor, gDhgrMono };
    bool had = readProfile(name, old);
    if (memcmp(had ? old : general, v, sizeof(old)) == 0) return;
    // Recopie des autres jeux, puis la ligne de celui-ci
    string all;
    FILE* f = fopen(GAMES_FILE, "r");
    if (f) {
        char line[GAMES_LINE];
        while (fgets(line, sizeof(line), f)) {
            char* bar = strrchr(line, '|');
            if (bar && name.compare(0, string::npos, line, bar - line) == 0) continue;
            all += line;
        }
        fclose(f);
    }
    char line[GAMES_LINE];
    snprintf(line, sizeof(line), "%s|%d %d %d %d\n", name.c_str(), v[0], v[1], v[2], v[3]);
    all += line;
    f = fopen(GAMES_FILE, "w");
    if (!f) return;
    fwrite(all.data(), 1, all.size(), f);
    fclose(f);
}

void Config::switchGame(const string& name) {
    currentGame = perGame ? name : "";
    model = gModel; joystick = gJoystick; monitor = gMonitor; dhgrMono = gDhgrMono;
    int v[4];
    if (!currentGame.empty() && readProfile(currentGame, v)) {
        if (v[0] >= 0 && v[0] < A2::MODEL_COUNT) model = (uint8_t)v[0];
        joystick = v[1] ? 1 : 0;
        if (v[2] >= 0 && v[2] < MONITOR_COUNT) monitor = (uint8_t)v[2];
        dhgrMono = v[3] ? 1 : 0;
    }
}
string Config::lastDir = "/sd";
string Config::disk[2] = { "", "" };
string Config::hdd = "";
bool Config::dirty = false;

static void readString(nvs_handle_t h, const char* key, string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) return;
    string buf(len, '\0');
    if (nvs_get_str(h, key, &buf[0], &len) == ESP_OK) out = buf.c_str();
}

void Config::load() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u8(h, "model", &model);
    nvs_get_u8(h, "keyb", &keyLayout);
    nvs_get_u8(h, "joystick", &joystick);
    nvs_get_u8(h, "monitor", &monitor);
    nvs_get_u8(h, "scanlines", &scanlines);
    nvs_get_u8(h, "dhgrmono", &dhgrMono);
    nvs_get_u8(h, "fringes", &fringes);
    nvs_get_u8(h, "charset", &charset);
    nvs_get_u8(h, "fastdisk", &fastDisk);
    nvs_get_u8(h, "volume", &volume);
    nvs_get_u8(h, "mockingb", &mockingboard);
    nvs_get_u8(h, "mouse", &mouse);
    nvs_get_u8(h, "lang", &language);
    nvs_get_u8(h, "info", &screenInfo);
    nvs_get_u8(h, "pergame", &perGame);
    readString(h, "lastdir", lastDir);
    readString(h, "disk1", disk[0]);
    readString(h, "disk2", disk[1]);
    readString(h, "hdd", hdd);
    // Derniers fichiers : un chemin par ligne
    string list;
    readString(h, "recent", list);
    recent.clear();
    for (size_t start = 0; start < list.size();) {
        size_t end = list.find('\n', start);
        if (end == string::npos) end = list.size();
        if (end > start) recent.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    nvs_close(h);

    // Valeurs hors bornes (flash d'une autre version) : retour aux valeurs par défaut
    if (model >= A2::MODEL_COUNT) model = A2::MODEL_IIE_ENH;
    if (keyLayout >= KEYB_COUNT) keyLayout = KEYB_FR;
    if (monitor >= MONITOR_COUNT) monitor = MONITOR_COLOR;
    if (volume > 8) volume = 4;
    if (screenInfo > 2) screenInfo = 1;
    charset = charset ? 1 : 0;
    language = language ? 1 : 0;
    if (lastDir.empty()) lastDir = "/sd";
    gModel = model; gJoystick = joystick; gMonitor = monitor; gDhgrMono = dhgrMono;
}

void Config::save() {
    dirty = false;
    // Un jeu est en place avec ses réglages : ils vont dans son fichier, et les
    // réglages généraux ne bougent pas
    if (perGame && !currentGame.empty()) {
        int v[4] = { model, joystick, monitor, dhgrMono };
        writeProfile(currentGame, v);
    } else {
        gModel = model; gJoystick = joystick; gMonitor = monitor; gDhgrMono = dhgrMono;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "model", gModel);
    nvs_set_u8(h, "keyb", keyLayout);
    nvs_set_u8(h, "joystick", gJoystick);
    nvs_set_u8(h, "monitor", gMonitor);
    nvs_set_u8(h, "scanlines", scanlines);
    nvs_set_u8(h, "dhgrmono", gDhgrMono);
    nvs_set_u8(h, "fringes", fringes);
    nvs_set_u8(h, "pergame", perGame);
    nvs_set_u8(h, "charset", charset);
    nvs_set_u8(h, "fastdisk", fastDisk);
    nvs_set_u8(h, "volume", volume);
    nvs_set_u8(h, "mockingb", mockingboard);
    nvs_set_u8(h, "mouse", mouse);
    nvs_set_u8(h, "lang", language);
    nvs_set_u8(h, "info", screenInfo);
    nvs_set_str(h, "lastdir", lastDir.c_str());
    nvs_set_str(h, "disk1", disk[0].c_str());
    nvs_set_str(h, "disk2", disk[1].c_str());
    nvs_set_str(h, "hdd", hdd.c_str());
    string list;
    for (const string& path : recent) list += path + "\n";
    nvs_set_str(h, "recent", list.c_str());
    nvs_commit(h);
    nvs_close(h);
}

#define RECENT_MAX 8

void Config::addRecent(const string& path) {
    for (size_t i = 0; i < recent.size(); i++)
        if (recent[i] == path) { recent.erase(recent.begin() + i); break; }
    recent.insert(recent.begin(), path);
    if (recent.size() > RECENT_MAX) recent.resize(RECENT_MAX);
    dirty = true;
}

void Config::saveIfDirty() {
    if (dirty) save();
}
