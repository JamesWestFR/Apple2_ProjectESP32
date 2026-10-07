/*

Apple2_ProjectESP32, an Apple II emulator for Espressif ESP32 SoC

Initialisation et boucle principale. Architecture reprise d'ESPectrum
(Víctor Iborra, David Crespo) : https://github.com/EremusOne/ESPectrum

Le cœur 0 émule : une image de l'Apple (17 030 cycles de 6502, rendu compris)
à chaque retour vertical de la sortie VGA, qui bat à 59,94 Hz comme un Apple
NTSC. Le cœur 1 porte la tâche audio et l'interruption de la sortie VGA.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "pwm_audio.h"
#include "driver/timer.h"
#include "Emu.h"
#include "ESPConfig.h"
#include "hardpins.h"
#include "Video.h"
#include "A2Keyboard.h"
#include "OSD.h"
#include "FileUtils.h"
#include "DiskImage.h"
#include "SerialConsole.h"
#include "core/A2.h"

namespace Emu {

fabgl::PS2Controller PS2Controller;

volatile bool paused = false;
bool turbo = false;
bool resetRequest = false;
bool coldBootRequest = false;
bool screenshotRequest = false;
bool forceRedraw = false;
float statFps = 0, statEmulatedFps = 0, statFrameMs = 0;

static TaskHandle_t audioTaskHandle = NULL;
static uint8_t* audioMix = nullptr;      // son de la dernière image, rempli par l'émulation
static uint8_t* audioOut = nullptr;      // copie envoyée à la sortie
static volatile int audioRateWanted = 0; // fréquence d'échantillonnage demandée (0 : inchangée)

static char noticeText[40] = "";
static int noticeFrames = 0;

void showNotice(const char* text) {
    snprintf(noticeText, sizeof(noticeText), " %s ", text);
    noticeFrames = 90;
}

}

// Requis par pwm_audio.c : son interruption appelle ce symbole (entrée cassette réelle, non gérée)
extern "C" void IRAM_ATTR AudioInGetAudio() { }

namespace Emu {

// Tâche audio (cœur 1) : à chaque image émulée, envoie ses échantillons à
// pwm_audio, dont l'écriture bloque quand son tampon est plein. Sans image
// reçue (menu, pause), elle sort du silence.
static void audioTask(void* unused) {
    pwm_audio_config_t pac;
    pac.duty_resolution    = LEDC_TIMER_8_BIT;
    pac.gpio_num_left      = SPEAKER_PIN;
    pac.ledc_channel_left  = LEDC_CHANNEL_0;
    pac.gpio_num_right     = -1;
    pac.ledc_channel_right = LEDC_CHANNEL_1;
    pac.ledc_timer_sel     = LEDC_TIMER_0;
    pac.tg_num             = TIMER_GROUP_0;
    pac.timer_num          = TIMER_0;
    pac.ringbuf_len        = 3072;
    pwm_audio_init(&pac);
    pwm_audio_set_param(AUDIO_RATE, LEDC_TIMER_8_BIT, 1);
    pwm_audio_start();
    pwm_audio_set_volume(0);

    size_t written;
    for (;;) {
        bool received = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60)) != 0;
        if (audioRateWanted) {
            pwm_audio_set_sample_rate(audioRateWanted);
            audioRateWanted = 0;
        }
        if (received && !OSD::active && !paused) memcpy(audioOut, audioMix, AUDIO_SAMPLES_PER_FRAME);
        else memset(audioOut, 128, AUDIO_SAMPLES_PER_FRAME);
        // Le délai doit valoir au moins un tick, sinon l'écriture ne bloque pas
        // et les échantillons qui ne tiennent pas dans le tampon sont perdus
        pwm_audio_write(audioOut, AUDIO_SAMPLES_PER_FRAME, &written, pdMS_TO_TICKS(100));
    }
}

void setup() {
    printf("\n| Apple2_ProjectESP32: booting\n");

    // Les 64 Ko de RAM principale d'abord, tant que la mémoire interne a un
    // bloc de cette taille : le framebuffer la morcelle ensuite
    uint8_t* mainRam = (uint8_t*)heap_caps_malloc(0x10000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // RAM auxiliaire du //e (80 colonnes, 128 Ko) : en PSRAM
    uint8_t* auxRam = (uint8_t*)heap_caps_malloc(0x10000, MALLOC_CAP_SPIRAM);
    // MALLOC_CAP_8BIT obligatoire : sans lui un tampon peut tomber en IRAM
    audioMix = (uint8_t*)heap_caps_malloc(AUDIO_SAMPLES_PER_FRAME, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    audioOut = (uint8_t*)heap_caps_malloc(AUDIO_SAMPLES_PER_FRAME, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mainRam || !audioMix || !audioOut) {
        printf("| Apple2_ProjectESP32: not enough internal memory\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    memset(audioMix, 128, AUDIO_SAMPLES_PER_FRAME);

    Config::load();

    // Révision de la puce : choisit les paramètres APLL de l'horloge vidéo (I2S.cpp)
    esp_chip_info_t chipInfo;
    esp_chip_info(&chipInfo);
    Config::esp32rev = chipInfo.revision;

    Video::init();
    FileUtils::initSD();
    printf("| Apple2_ProjectESP32: SD %s\n", FileUtils::SDReady ? "mounted" : "NOT mounted");

    A2::init(mainRam, auxRam);
    A2::setModel((A2::Model)Config::model);
    A2::Mockingboard::setEnabled(Config::mockingboard);

    // Disquettes restées dans les lecteurs. Une image supprimée ou illisible
    // est oubliée plutôt que réessayée à chaque démarrage.
    for (int d = 0; d < 2; d++) {
        if (!FileUtils::SDReady || Config::disk[d].empty()) continue;
        if (!DiskImage::insert(d, Config::disk[d])) {
            Config::disk[d] = "";
            Config::save();
        }
    }

    if (FileUtils::SDReady && !Config::hdd.empty() && !DiskImage::insert(0, Config::hdd)) {
        Config::hdd = "";
        Config::save();
    }

    // PS/2 (FabGL) : le clavier sur le port 0
    PS2Controller.begin(fabgl::PS2Preset::KeyboardPort0, fabgl::KbdMode::CreateVirtualKeysQueue);

    xTaskCreatePinnedToCore(&audioTask, "audioTask", 2048, NULL, configMAX_PRIORITIES - 1, &audioTaskHandle, 1);

    printf("| Apple2_ProjectESP32: %s ready, chip rev %u, PSRAM %u KB, aux RAM %s, free heap %u KB\n",
           A2::modelName(A2::model()), (unsigned)Config::esp32rev,
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024), auxRam ? "yes" : "no",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
}

// Attend le retour vertical de la sortie VGA. Sans signal (VGA non démarrée),
// la durée d'une image fait l'affaire.
static void waitVsync(int64_t frameStart) {
    int64_t limit = frameStart + 2 * MICROS_PER_FRAME;
    while (!Video::vsync && esp_timer_get_time() < limit) {}
    Video::vsync = false;
}

static void takeScreenshot() {
    if (!FileUtils::SDReady) { showNotice(T("PAS DE CARTE SD", "NO SD CARD")); return; }
    char path[40];
    for (int n = 1; n < 100; n++) {
        snprintf(path, sizeof(path), "/sd/A2SHOT%02d.BMP", n);
        FILE* f = fopen(path, "rb");
        if (!f) {
            showNotice(Video::screenshot(path) ? path + 4 : T("ECRITURE IMPOSSIBLE", "WRITE FAILED"));
            return;
        }
        fclose(f);
    }
}

void loop() {
    // La boucle ne dort jamais (elle attend le retour vertical de l'écran) : la
    // tâche IDLE de ce cœur ne tourne plus, on la retire donc de la
    // surveillance du chien de garde
    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));

    int64_t frameStart = esp_timer_get_time();
    int64_t statStart = frameStart, statCpu = 0;
    int statFrames = 0, statEmulated = 0, statRendered = 0, statSeconds = 0;
    int64_t rateStart = 0;
    int rateFrames = 0, rateNow = AUDIO_RATE;
    bool pauseShown = false;

    for (;;) {
        Keyb::process();
        SerialConsole::tick();

        if (coldBootRequest) {
            coldBootRequest = resetRequest = false;
            A2::Disk::flush();
            A2::powerOn();
            Keyb::releaseAll();
        }
        if (resetRequest) {
            resetRequest = false;
            A2::reset();
        }

        bool frozen = OSD::active || paused;
        if (paused && !pauseShown && !OSD::active) Video::overlayText(17, 11, " PAUSE ");
        pauseShown = paused;

        if (!frozen) {
            int64_t t0 = esp_timer_get_time();
            A2::renderEnabled = true;
            if (forceRedraw) A2::videoInvalidate();
            A2::runFrame();
            A2::renderAudio(audioMix, AUDIO_SAMPLES_PER_FRAME, Config::volume);
            if (audioTaskHandle) xTaskNotifyGive(audioTaskHandle);
            int64_t cpu = esp_timer_get_time() - t0;
            statCpu += cpu;
            statEmulated++;
            statRendered++;

            // Lecteur en marche ou vitesse maximale : d'autres images, sans
            // rendu ni son, tant qu'elles tiennent avant le prochain retour vertical
            if (turbo || (Config::fastDisk && A2::Disk::busy())) {
                A2::renderEnabled = false;
                while (esp_timer_get_time() - t0 + cpu < MICROS_PER_FRAME - 1500) {
                    A2::runFrame();
                    statEmulated++;
                    if (!turbo && !A2::Disk::busy()) break;
                }
                A2::renderEnabled = true;
                rateFrames = 0;
            } else if (rateFrames++ == 0) {
                rateStart = t0;
            } else if (rateFrames > 120) {
                // La sortie son suit la cadence réelle des images, mesurée sur deux secondes
                int rate = (int)(120LL * AUDIO_SAMPLES_PER_FRAME * 1000000 / (t0 - rateStart));
                if (rate > 28000 && rate < 34000 && abs(rate - rateNow) > 15) {
                    rateNow = rate;
                    audioRateWanted = rate;
                }
                rateFrames = 0;
            }

            // Voyants des lecteurs, en bas à droite
            if (Config::screenInfo) {
                if (A2::Disk::activity[0]) Video::overlayText(38, 23, "1");
                if (A2::Disk::activity[1]) Video::overlayText(39, 23, "2");
                if (A2::Hdd::activity) Video::overlayText(37, 23, "H");
            }
            // Vitesse : images par seconde et part du temps d'une image prise par l'émulation
            if (Config::screenInfo == 2) {
                char speed[16];
                snprintf(speed, sizeof(speed), "%4.1f %3d%%", statFps, (int)(statFrameMs * 100000 / MICROS_PER_FRAME));
                Video::overlayText(40 - (int)strlen(speed), 0, speed);
            }
            if (noticeFrames) {
                noticeFrames--;
                Video::overlayText(0, 0, noticeText);
            }
        }

        if (screenshotRequest) {
            screenshotRequest = false;
            takeScreenshot();
        }

        waitVsync(frameStart);
        frameStart = esp_timer_get_time();

        // Vitesse, mesurée chaque seconde ; écrite sur le port série une fois par minute
        if (++statFrames == 60) {
            float seconds = (frameStart - statStart) / 1.0e6f;
            statFps = statFrames / seconds;
            statEmulatedFps = statEmulated / seconds;
            if (statRendered) statFrameMs = statCpu / 1000.0f / statRendered;
            statStart = frameStart;
            statCpu = 0;
            statFrames = statEmulated = statRendered = 0;
            if (++statSeconds == 60) {
                statSeconds = 0;
                printf("Apple2_ProjectESP32: %.1f images/s, %.1f emulated/s, %.2f ms/image (budget %.2f), free heap %u KB\n",
                       statFps, statEmulatedFps, statFrameMs, MICROS_PER_FRAME / 1000.0,
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
            }
        }
    }
}

}
