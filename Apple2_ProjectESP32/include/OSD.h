/*

Apple2_ProjectESP32 — Menu à l'écran et sélecteur de fichiers.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef OSD_h
#define OSD_h

#include <stdint.h>
#include "fabgl.h"

namespace OSD {

enum { MENU_MAIN = 0, MENU_FILES };

// Menu affiché : l'émulation est figée
extern bool active;

// Ouvre le menu demandé, ou le ferme s'il est déjà ouvert
void toggle(int menu);
void key(fabgl::VirtualKey vk, uint8_t ascii);

}

#endif // OSD_h
