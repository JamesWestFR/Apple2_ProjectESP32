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
#include <string>
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

using namespace std;

namespace Emu {

fabgl::PS2Controller PS2Controller;

volatile bool paused = false;
bool turbo = false;
bool resetRequest = false;
bool coldBootRequest = false;
bool screenshotRequest = false;
uint8_t stateRequest = 0;
bool forceRedraw = false;
float statFps = 0, statEmulatedFps = 0, statFrameMs = 0;

static bool mouseAtBoot = false;    // le port souris PS/2 a été initialisé
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
    // Extension mémoire du //c (ROM 3 et 4) : 1 Mo de PSRAM
    uint8_t* expansion = (uint8_t*)heap_caps_calloc(1, 0x100000, MALLOC_CAP_SPIRAM);
    A2::setExpansionRam(expansion, expansion ? 0x100000 : 0);
    A2::setModel((A2::Model)Config::model);
    A2::Mockingboard::setEnabled(Config::mockingboard);
    A2::Mouse::setEnabled(Config::mouse);

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

    // PS/2 (FabGL) : le clavier sur le port 0 et, si la carte souris est en
    // place, une souris sur le port 1. Sans souris branchée, son initialisation
    // attend une réponse pendant une seconde environ.
    mouseAtBoot = Config::mouse != 0;
    PS2Controller.begin(mouseAtBoot ? fabgl::PS2Preset::KeyboardPort0_MousePort1 : fabgl::PS2Preset::KeyboardPort0,
                        fabgl::KbdMode::CreateVirtualKeysQueue);
    if (mouseAtBoot)
        printf("| Apple2_ProjectESP32: PS/2 mouse %s\n",
               PS2Controller.mouse() && PS2Controller.mouse()->isMouseAvailable() ? "found" : "NOT found");

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

// Réglages par jeu : ceux de l'image en place (le disque dur d'abord, sinon la
// disquette du lecteur 1), ou les réglages généraux
string gameName() {
    return !DiskImage::hddPath().empty() ? DiskImage::hddName() : DiskImage::name(0);
}

static void applyGameSettings() {
    Config::switchGame(gameName());
    Video::setMonitor(Config::monitor);
    if (Config::model != A2::model()) A2::setModel((A2::Model)Config::model);
}

// Souris PS/2 -> carte souris : déplacements et boutons reçus depuis la dernière image
static void pollMouse() {
    if (!mouseAtBoot) return;
    fabgl::Mouse* mouse = PS2Controller.mouse();
    if (!mouse) return;
    fabgl::MouseDelta delta;
    int dx = 0, dy = 0;
    bool any = false;
    for (int guard = 0; guard < 16 && mouse->deltaAvailable(); guard++) {
        if (!mouse->getNextDelta(&delta, 0)) break;
        dx += delta.deltaX;
        dy -= delta.deltaY;         // le PS/2 compte vers le haut, l'Apple vers le bas
        any = true;
    }
    if (!any || OSD::active || paused) return;
    A2::Mouse::move(dx, dy);
    A2::Mouse::setButton(0, delta.buttons.left);
    A2::Mouse::setButton(1, delta.buttons.right);
}

// Imprimante du //c (port série 1) : ce qu'elle reçoit est ajouté à PRINTER.TXT,
// écrit sur la carte SD quand l'impression s'arrête
#define PRINTER_BUFFER 512
static char printerBuffer[PRINTER_BUFFER];
static int printerCount = 0, printerIdle = 0;

static void printerFlush() {
    if (printerCount == 0) return;
    FILE* f = FileUtils::SDReady ? fopen("/sd/PRINTER.TXT", "a") : nullptr;
    if (f) {
        fwrite(printerBuffer, 1, printerCount, f);
        fclose(f);
    }
    printerCount = 0;
}

static bool stateWrite(void* ctx, void* data, uint32_t len) { return fwrite(data, 1, len, (FILE*)ctx) == len; }
static bool stateRead(void* ctx, void* data, uint32_t len) { return fread(data, 1, len, (FILE*)ctx) == len; }

// L'état est rangé à côté de l'image en place, sous son nom : JEU.DSK -> JEU.A2S
// (le disque dur d'abord, sinon la disquette du lecteur 1)
static string statePath() {
    string base = !DiskImage::hddPath().empty() ? DiskImage::hddPath()
                : !DiskImage::path(0).empty() ? DiskImage::path(0) : FileUtils::MountPoint + "/APPLE2.";
    size_t dot = base.find_last_of('.');
    if (dot != string::npos && dot > base.find_last_of('/')) base = base.substr(0, dot);
    return base + ".A2S";
}

static void saveOrLoadState(bool saving) {
    if (!FileUtils::SDReady) { showNotice(T("PAS DE CARTE SD", "NO SD CARD")); return; }
    string path = statePath();
    FILE* f = fopen(path.c_str(), saving ? "wb" : "rb");
    if (!f) { showNotice(saving ? T("ECRITURE IMPOSSIBLE", "WRITE FAILED") : T("PAS D'ETAT SAUVE", "NO SAVED STATE")); return; }
    bool ok;
    if (saving) {
        ok = A2::saveState(stateWrite, f);
    } else {
        A2::Disk::flush();
        ok = A2::loadState(stateRead, f);
        // L'état reprend son modèle d'Apple, jusqu'au prochain changement au menu
        Config::model = (uint8_t)A2::model();
        Keyb::releaseAll();
    }
    fclose(f);
    if (saving && !ok) remove(path.c_str());
    showNotice(!ok ? (saving ? T("ECRITURE IMPOSSIBLE", "WRITE FAILED") : T("ETAT ILLISIBLE", "BAD STATE FILE"))
                   : saving ? T("ETAT SAUVE", "STATE SAVED") : T("ETAT REPRIS", "STATE LOADED"));
    printf("| Apple2_ProjectESP32: state %s %s: %s\n", saving ? "save" : "load", path.c_str(), ok ? "ok" : "FAILED");
}

void loop() {
    // La boucle ne dort jamais (elle attend le retour vertical de l'écran) : la
    // tâche IDLE de ce cœur ne tourne plus, on la retire donc de la
    // surveillance du chien de garde
    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));

    applyGameSettings();

    int64_t frameStart = esp_timer_get_time();
    int64_t statStart = frameStart, statCpu = 0;
    int statFrames = 0, statEmulated = 0, statRendered = 0, statSeconds = 0;
    int64_t rateStart = 0;
    int rateFrames = 0, rateNow = AUDIO_RATE;
    bool pauseShown = false;

    for (;;) {
        Keyb::process();
        SerialConsole::tick();
        pollMouse();

        if (coldBootRequest) {
            coldBootRequest = resetRequest = false;
            A2::Disk::flush();
            // Les réglages du jeu en place entrent en vigueur au démarrage de l'Apple
            applyGameSettings();
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
        // Une demi-seconde sans rien recevoir : l'impression est finie
        if (printerCount && ++printerIdle > 30) printerFlush();
        if (stateRequest) {
            if (!OSD::active) saveOrLoadState(stateRequest == 1);
            stateRequest = 0;
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

void A2_platformSerialOut(int port, uint8_t value) {
    if (port != 0) return;
    // L'Apple émet souvent avec le bit 7 levé ; fin de ligne : retour chariot
    char c = (char)(value & 0x7F);
    if (c == 0x0D) c = '\n';
    Emu::printerBuffer[Emu::printerCount++] = c;
    Emu::printerIdle = 0;
    if (Emu::printerCount >= PRINTER_BUFFER) Emu::printerFlush();
}
