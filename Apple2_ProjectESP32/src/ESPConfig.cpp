/*

Apple2_ProjectESP32 — Réglages mémorisés en NVS.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

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
uint8_t Config::fastDisk = 1;
uint8_t Config::volume = 4;
uint8_t Config::mockingboard = 1;
uint8_t Config::language = 0;
uint8_t Config::screenInfo = 1;
vector<string> Config::recent;
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
    nvs_get_u8(h, "fastdisk", &fastDisk);
    nvs_get_u8(h, "volume", &volume);
    nvs_get_u8(h, "mockingb", &mockingboard);
    nvs_get_u8(h, "lang", &language);
    nvs_get_u8(h, "info", &screenInfo);
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
    language = language ? 1 : 0;
    if (lastDir.empty()) lastDir = "/sd";
}

void Config::save() {
    dirty = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "model", model);
    nvs_set_u8(h, "keyb", keyLayout);
    nvs_set_u8(h, "joystick", joystick);
    nvs_set_u8(h, "monitor", monitor);
    nvs_set_u8(h, "scanlines", scanlines);
    nvs_set_u8(h, "fastdisk", fastDisk);
    nvs_set_u8(h, "volume", volume);
    nvs_set_u8(h, "mockingb", mockingboard);
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
