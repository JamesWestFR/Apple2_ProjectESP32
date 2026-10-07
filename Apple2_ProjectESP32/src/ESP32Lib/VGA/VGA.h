/*
	Author: bitluni 2019
	License:
	Creative Commons Attribution ShareAlike 4.0
	https://creativecommons.org/licenses/by-sa/4.0/

	For further details check out:
		https://youtube.com/bitlunislab
		https://github.com/bitluni
		http://bitluni.net
*/
#pragma once

#include "../I2S/I2S.h"

enum vmodeproperties {
	hFront,	hSync, hBack, hRes,	vFront,	vSync, vBack, vRes,	vDiv, hSyncPolarity, vSyncPolarity,	r1sdm0,	r1sdm1,	r1sdm2,	r1odiv,	r0sdm2,	r0odiv
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGA STANDARD MODES (IN STANDARD MODE TIMING DEPENDS ON PWM_AUDIO SO ALL MACHINES SHARE SAME VIDEO MODES)
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Apple II : signal VGA 640x480 à 60 Hz (horloge de points 25,175 MHz, 31,47 kHz,
// 59,94 Hz), dont l'image n'occupe que 560 points sur 640 et 384 lignes sur 480.
// Les marges sont du noir fourni par les paliers : le framebuffer ne contient que
// les 560 x 192 points de l'Apple, chaque ligne étant envoyée deux fois.
//   horizontal : 16 + 40 de marge, synchro 96, 48 + 40 de marge, 560 points
//   vertical   : 10 + 48 de marge, synchro 2, 33 + 48 de marge, 384 lignes
// APLL : 40 MHz x (4 + 6 + 17/256 + 236/65536) / (2 x (2 + 2)) = 50,35 MHz, soit
// deux fois l'horloge de points.
#define VgaMode_560x192_A2 { 56, 96, 88, 560, 58, 2, 81, 384, 2, 1, 1, 236,17,6,2,6,2 }

#define VGA_STRETCHED_LINES 272

const unsigned short int vidmodes[1][17]={
	VgaMode_560x192_A2
};

class VGA : public I2S {

  public:

	VGA(const int i2sIndex = 0);

	bool init(int mode, const int *pinMap, const int bitCount, const int clockPin = -1);

	int mode;

	int CenterH = 0;
	int CenterV = 0;

  protected:

	virtual void initSyncBits() = 0;
	virtual long syncBits(bool h, bool v) = 0;

	long vsyncBit;
	long hsyncBit;
	long vsyncBitI;
	long hsyncBitI;

	virtual void allocateLineBuffers();
	virtual void allocateLineBuffers(void **frameBuffer);
	virtual void propagateResolution(const int xres, const int yres) = 0;

};
