/*

Apple2_ProjectESP32 — Carte SD : montage (SPI) et liste de fichiers (adapté d'ESPectrum)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <algorithm>
#include "FileUtils.h"
#include "hardpins.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#define MOUNT_POINT_SD "/sd"
#define SDCARD_HOST_MAXFREQ 20000

string FileUtils::MountPoint = MOUNT_POINT_SD;
bool FileUtils::SDReady = false;
bool FileUtils::listTruncated = false;

static sdmmc_card_t* card = nullptr;

string FileUtils::getLCaseExt(const string& filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == string::npos) return "";
    string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = tolower(c);
    return ext;
}

void FileUtils::initSD() {
    if (!SDReady)
        SDReady = mountSDCard(PIN_NUM_MISO_LILYGO_ESPECTRUM, PIN_NUM_MOSI_LILYGO_ESPECTRUM,
                              PIN_NUM_CLK_LILYGO_ESPECTRUM, PIN_NUM_CS_LILYGO_ESPECTRUM);
}

bool FileUtils::mountSDCard(int PIN_MISO, int PIN_MOSI, int PIN_CLK, int PIN_CS) {
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH1);
    if (ret != ESP_OK) {
        printf("SD Card init: Failed to initialize bus.\n");
        return false;
    }

    sdspi_device_config_t slot_config = {
        .host_id   = SPI2_HOST,
        .gpio_cs   = (gpio_num_t)PIN_CS,
        .gpio_cd   = SDSPI_SLOT_NO_CD,
        .gpio_wp   = SDSPI_SLOT_NO_WP,
        .gpio_int  = GPIO_NUM_NC,
    };

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT_SD, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL)
            printf("Failed to mount filesystem.\n");
        else
            printf("Failed to initialize the SD card.\n");
        spi_bus_free(SPI2_HOST);
        return false;
    }

    host.max_freq_khz = SDCARD_HOST_MAXFREQ;
    host.set_card_clk(host.slot, SDCARD_HOST_MAXFREQ);

    printf("SD Card mounted.\n");
    sdmmc_card_print_info(stdout, card);

    return true;
}

vector<string> FileUtils::listFiles(const string& path, const string& exts) {
    vector<string> dirs, files;
    listTruncated = false;
    DIR* dir = opendir(path.c_str());
    if (!dir) return dirs;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if ((int)(dirs.size() + files.size()) >= LIST_MAX) { listTruncated = true; break; }
        string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        // Dossier que Windows crée sur chaque carte
        if (name == "System Volume Information") continue;
        if (de->d_type == DT_DIR) {
            dirs.push_back(name + "/");
            continue;
        }
        string ext = getLCaseExt(name);
        if (!ext.empty() && ("," + exts + ",").find("," + ext + ",") != string::npos)
            files.push_back(name);
    }
    closedir(dir);
    auto noCase = [](const string& a, const string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; };
    sort(dirs.begin(), dirs.end(), noCase);
    sort(files.begin(), files.end(), noCase);
    dirs.insert(dirs.end(), files.begin(), files.end());
    return dirs;
}
