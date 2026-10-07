/*

Apple2_ProjectESP32 — Clavier PS/2 -> clavier de l'Apple, manette au clavier.

Le clavier de l'Apple ne donne pas une matrice de touches mais un code ASCII.
La touche du PC est donc traduite en caractère, selon la disposition choisie
(AZERTY ou QWERTY) : la touche marquée A donne un A. La traduction part du
code de la touche (scancode), pas des touches virtuelles de FabGL, qui ne
connaît ici que la disposition américaine.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "A2Keyboard.h"
#include "Emu.h"
#include "ESPConfig.h"
#include "OSD.h"
#include "core/A2.h"

using namespace fabgl;

namespace Keyb {

// Caractères d'une touche selon la disposition : sans Maj, avec Maj, avec AltGr.
// 0 : rien (lettre accentuée, que l'Apple américain ne connaît pas).
struct KeyDef {
    uint8_t scancode;      // jeu de scancodes 2 du PS/2
    char us, usShift;
    char fr, frShift, frAltGr;
};

static const KeyDef keyDefs[] = {
    { 0x0E, '`', '~',   0,   0,   0   },
    { 0x16, '1', '!',   '&', '1', 0   },
    { 0x1E, '2', '@',   0,   '2', '~' },
    { 0x26, '3', '#',   '"', '3', '#' },
    { 0x25, '4', '$',   '\'', '4', '{' },
    { 0x2E, '5', '%',   '(', '5', '[' },
    { 0x36, '6', '^',   '-', '6', '|' },
    { 0x3D, '7', '&',   0,   '7', '`' },
    { 0x3E, '8', '*',   '_', '8', '\\' },
    { 0x46, '9', '(',   0,   '9', '^' },
    { 0x45, '0', ')',   0,   '0', '@' },
    { 0x4E, '-', '_',   ')', 0,   ']' },
    { 0x55, '=', '+',   '=', '+', '}' },
    { 0x15, 'q', 'Q',   'a', 'A', 0   },
    { 0x1D, 'w', 'W',   'z', 'Z', 0   },
    { 0x24, 'e', 'E',   'e', 'E', 0   },
    { 0x2D, 'r', 'R',   'r', 'R', 0   },
    { 0x2C, 't', 'T',   't', 'T', 0   },
    { 0x35, 'y', 'Y',   'y', 'Y', 0   },
    { 0x3C, 'u', 'U',   'u', 'U', 0   },
    { 0x43, 'i', 'I',   'i', 'I', 0   },
    { 0x44, 'o', 'O',   'o', 'O', 0   },
    { 0x4D, 'p', 'P',   'p', 'P', 0   },
    { 0x54, '[', '{',   '^', 0,   0   },
    { 0x5B, ']', '}',   '$', 0,   0   },
    { 0x1C, 'a', 'A',   'q', 'Q', 0   },
    { 0x1B, 's', 'S',   's', 'S', 0   },
    { 0x23, 'd', 'D',   'd', 'D', 0   },
    { 0x2B, 'f', 'F',   'f', 'F', 0   },
    { 0x34, 'g', 'G',   'g', 'G', 0   },
    { 0x33, 'h', 'H',   'h', 'H', 0   },
    { 0x3B, 'j', 'J',   'j', 'J', 0   },
    { 0x42, 'k', 'K',   'k', 'K', 0   },
    { 0x4B, 'l', 'L',   'l', 'L', 0   },
    { 0x4C, ';', ':',   'm', 'M', 0   },
    { 0x52, '\'', '"',  0,   '%', 0   },
    { 0x5D, '\\', '|',  '*', 0,   0   },
    { 0x61, '\\', '|',  '<', '>', 0   },     // touche à gauche du W (claviers européens)
    { 0x1A, 'z', 'Z',   'w', 'W', 0   },
    { 0x22, 'x', 'X',   'x', 'X', 0   },
    { 0x21, 'c', 'C',   'c', 'C', 0   },
    { 0x2A, 'v', 'V',   'v', 'V', 0   },
    { 0x32, 'b', 'B',   'b', 'B', 0   },
    { 0x31, 'n', 'N',   'n', 'N', 0   },
    { 0x3A, 'm', 'M',   ',', '?', 0   },
    { 0x41, ',', '<',   ';', '.', 0   },
    { 0x49, '.', '>',   ':', '/', 0   },
    { 0x4A, '/', '?',   '!', 0,   0   },
    { 0x29, ' ', ' ',   ' ', ' ', 0   },
};

// État de la manette au clavier et des boutons
static bool joyLeft, joyRight, joyUp, joyDown;
static bool altLeft, altRight, guiKey, ctrlRight, shiftRight;
static uint8_t heldScancode = 0;      // touche dont l'appui a envoyé un caractère
static VirtualKey heldVk = VK_NONE;

static void updateButtons() {
    bool joy = Config::joystick != 0;
    // Pomme ouverte : Alt gauche. Pomme pleine : touche Windows ou Menu, et Alt
    // droite quand elle ne sert pas d'AltGr. Avec la manette au clavier, Ctrl
    // droite et Maj droite sont les deux boutons.
    A2::setButton(0, altLeft || (joy && ctrlRight));
    A2::setButton(1, guiKey || (altRight && Config::keyLayout == KEYB_US) || (joy && shiftRight));
}

static void updatePaddles() {
    A2::setPaddle(0, joyLeft ? 0 : (joyRight ? 255 : 127));
    A2::setPaddle(1, joyUp ? 0 : (joyDown ? 255 : 127));
}

void releaseAll() {
    joyLeft = joyRight = joyUp = joyDown = false;
    altLeft = altRight = guiKey = ctrlRight = shiftRight = false;
    heldScancode = 0;
    heldVk = VK_NONE;
    updateButtons();
    updatePaddles();
    A2::keyUp();
}

// Code de la touche pour une touche ordinaire (ni étendue, ni Pause), sinon 0
static uint8_t plainScancode(const VirtualKeyItem& item) {
    uint8_t first = item.scancode[0];
    if (first == 0xE0 || first == 0xE1 || first == 0xE2) return 0;
    return first == 0xF0 ? item.scancode[1] : first;
}

static uint8_t translate(const VirtualKeyItem& item) {
    uint8_t c = 0;
    switch (item.vk) {
        case VK_RETURN: case VK_KP_ENTER: c = 0x0D; break;
        case VK_ESCAPE: c = 0x1B; break;
        case VK_BACKSPACE: c = 0x08; break;
        case VK_DELETE: case VK_KP_DELETE: c = 0x7F; break;
        case VK_TAB: c = 0x09; break;
        case VK_LEFT: case VK_KP_LEFT: c = 0x08; break;
        case VK_RIGHT: case VK_KP_RIGHT: c = 0x15; break;
        case VK_UP: case VK_KP_UP: c = 0x0B; break;
        case VK_DOWN: case VK_KP_DOWN: c = 0x0A; break;
        case VK_KP_0: case VK_KP_1: case VK_KP_2: case VK_KP_3: case VK_KP_4:
        case VK_KP_5: case VK_KP_6: case VK_KP_7: case VK_KP_8: case VK_KP_9:
            c = '0' + (item.vk - VK_KP_0); break;
        case VK_KP_PERIOD: c = '.'; break;
        case VK_KP_PLUS: c = '+'; break;
        case VK_KP_MINUS: c = '-'; break;
        case VK_KP_MULTIPLY: c = '*'; break;
        case VK_KP_DIVIDE: c = '/'; break;
        default: break;
    }
    if (c) return c;

    uint8_t sc = plainScancode(item);
    if (!sc) return 0;
    for (const KeyDef& k : keyDefs) {
        if (k.scancode != sc) continue;
        if (Config::keyLayout == KEYB_FR) c = item.RALT ? k.frAltGr : (item.SHIFT ? k.frShift : k.fr);
        else c = item.SHIFT ? k.usShift : k.us;
        break;
    }
    if (!c) return 0;

    // Verr Maj éteint : majuscules, comme sur un Apple dont la touche est enfoncée
    if (!item.CAPSLOCK && c >= 'a' && c <= 'z') c -= 0x20;
    if (item.CTRL) {
        if (c >= 'a' && c <= 'z') c -= 0x20;
        if (c >= '@' && c <= '_') c &= 0x1F;
    }
    return c;
}

static void handle(const VirtualKeyItem& item) {
    const bool down = item.down;

    // --- Touches de l'émulateur ---
    if (down) {
        switch (item.vk) {
            case VK_F12:
                // Ctrl+F12 : arrêt et remise sous tension de l'Apple
                if (item.CTRL) Emu::coldBootRequest = true;
                else OSD::toggle(OSD::MENU_MAIN);
                return;
            case VK_F11:
                // Ctrl+F11 : Ctrl-Reset
                if (item.CTRL) Emu::resetRequest = true;
                else OSD::toggle(OSD::MENU_FILES);
                return;
            case VK_PAUSE:
                if (!OSD::active) Emu::paused = !Emu::paused;
                return;
            case VK_SCROLLLOCK:
                Emu::turbo = !Emu::turbo;
                Emu::showNotice(Emu::turbo ? T("VITESSE MAX", "FULL SPEED") : T("VITESSE NORMALE", "NORMAL SPEED"));
                return;
            case VK_PRINTSCREEN:
                Emu::screenshotRequest = true;
                return;
            case VK_F9:
                Config::joystick = !Config::joystick;
                Config::dirty = true;
                releaseAll();
                Emu::showNotice(Config::joystick ? T("MANETTE : FLECHES", "JOYSTICK: ARROWS") : T("MANETTE : NON", "JOYSTICK: OFF"));
                return;
            default: break;
        }
    }

    if (OSD::active) {
        if (down) OSD::key(item.vk, translate(item));
        return;
    }

    // --- Boutons ---
    switch (item.vk) {
        case VK_LALT: altLeft = down; updateButtons(); return;
        case VK_RALT: altRight = down; updateButtons(); return;
        case VK_LGUI: case VK_RGUI: case VK_APPLICATION: guiKey = down; updateButtons(); return;
        case VK_RCTRL: ctrlRight = down; updateButtons(); return;
        case VK_RSHIFT: shiftRight = down; updateButtons(); return;
        default: break;
    }

    // --- Manette sur les flèches ---
    if (Config::joystick) {
        switch (item.vk) {
            case VK_LEFT: case VK_KP_LEFT: joyLeft = down; updatePaddles(); return;
            case VK_RIGHT: case VK_KP_RIGHT: joyRight = down; updatePaddles(); return;
            case VK_UP: case VK_KP_UP: joyUp = down; updatePaddles(); return;
            case VK_DOWN: case VK_KP_DOWN: joyDown = down; updatePaddles(); return;
            default: break;
        }
    }

    // --- Clavier de l'Apple ---
    uint8_t sc = plainScancode(item);
    if (down) {
        uint8_t c = translate(item);
        if (!c) return;
        A2::keyDown(c);
        heldScancode = sc;
        heldVk = item.vk;
    } else if ((sc && sc == heldScancode) || (!sc && item.vk == heldVk)) {
        A2::keyUp();
        heldScancode = 0;
        heldVk = VK_NONE;
    }
}

void process() {
    VirtualKeyItem item;
    auto kbd = Emu::PS2Controller.keyboard();
    while (kbd && kbd->virtualKeyAvailable())
        if (kbd->getNextVirtualKey(&item)) handle(item);
}

}
