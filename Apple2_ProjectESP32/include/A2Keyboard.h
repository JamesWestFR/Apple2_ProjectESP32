/*

Apple2_ProjectESP32 — Clavier PS/2 -> clavier de l'Apple, manette au clavier.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef A2Keyboard_h
#define A2Keyboard_h

namespace Keyb {

void process();          // à appeler à chaque image : vide la file des touches
void releaseAll();       // touches et manette relâchées (ouverture du menu, reset)

}

#endif // A2Keyboard_h
