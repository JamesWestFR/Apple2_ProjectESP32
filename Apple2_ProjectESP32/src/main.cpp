/*

Apple2_ProjectESP32, an Apple II emulator for Espressif ESP32 SoC

Émulateur Apple ][, ][+, //e et //e Enhanced pour la carte LILYGO TTGO VGA32.

Copyright (c) 2026 — projet personnel et indépendant

Architecture et code de plateforme repris d'ESPectrum by Víctor Iborra [Eremus]
https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include "Emu.h"

#ifdef A2_WIFI_APP
// Second firmware (partition ota_1) : transfert de fichiers par WiFi, voir WifiApp.cpp
void WifiApp_run();
#endif

extern "C" void app_main(void) {

#ifdef A2_WIFI_APP
  WifiApp_run();
#else
  Emu::setup();

  Emu::loop();
#endif

}
