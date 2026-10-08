#!/usr/bin/env python3
"""
rom2header.py — Convertit les ROM Apple II de RomsApple/ en header C.

Génère Apple2_ProjectESP32/include/roms_apple2.h :
  - gb_rom_apple2        Apple ][ (Integer BASIC), 12 Ko, $D000-$FFFF
  - gb_rom_apple2plus    Apple ][+ (Applesoft, Autostart), 12 Ko, $D000-$FFFF
  - gb_rom_apple2e       Apple //e, 16 Ko, $C000-$FFFF
  - gb_rom_apple2e_enh   Apple //e Enhanced, 16 Ko, $C000-$FFFF
  - gb_rom_apple2c       Apple //c (ROM 255), 16 Ko, $C000-$FFFF
  - gb_rom_apple2c0, c3, c4, cp   Apple //c ROM 0, 3, 4 et //c Plus, 32 Ko en deux moitiés
  - gb_rom_disk2         carte Disk II 16 secteurs (P5), 256 octets, $Cn00
  - gb_rom_disk2_p6      carte Disk II, PROM P6 du séquenceur (lecture des images WOZ)
  - gb_rom_mouse         carte souris AppleMouse II, 2 Ko
  - gb_rom_video2e       générateur de caractères du //e, 2 Ko
  - gb_rom_video2e_enh   générateur de caractères du //e Enhanced (MouseText), 2 Ko
  - gb_rom_video2e_fr, gb_rom_video2e_enh_fr   les mêmes, en français

Usage (sans argument) : python scripts/rom2header.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_DIR = ROOT / "RomsApple"
OUT = ROOT / "Apple2_ProjectESP32" / "include" / "roms_apple2.h"

# (nom du tableau C, [fichiers concaténés], taille attendue, commentaire)
ROMS = [
    # $D000 : Programmer's Aid #1 ; $D800-$DFFF : support vide ; $E000-$FFFF : Integer BASIC et moniteur
    ("gb_rom_apple2", ["341-0016-00.d0", "vide:2048", "341-0001-00.e0", "341-0002-00.e8", "341-0003-00.f0", "341-0004-00.f8"],
     0x3000, "Apple ][ : Integer BASIC + moniteur, $D000-$FFFF"),
    ("gb_rom_apple2plus", ["341-0011.d0", "341-0012.d8", "341-0013.e0", "341-0014.e8", "341-0015.f0", "341-0020-00.f8"],
     0x3000, "Apple ][+ : Applesoft + Autostart, $D000-$FFFF"),
    ("gb_rom_apple2e",
     ["Apple IIe ROM Pages C0-DF - 342-0135-A - 1982.bin", "Apple IIe ROM Pages E0-FF - 342-0134-A - 1982.bin"],
     0x4000, "Apple //e, $C000-$FFFF"),
    ("gb_rom_apple2e_enh", ["Apple IIe Enhanced ROM Pages C0-FF - 342-0349-B - 1985.bin"],
     0x4000, "Apple //e Enhanced, $C000-$FFFF"),
    ("gb_rom_apple2c", ["apple2c/a2c.128"], 0x4000, "Apple //c, première ROM (version 255), $C000-$FFFF"),
    # ROM de 32 Ko : deux moitiés de 16 Ko, échangées par $C028
    ("gb_rom_apple2c0", ["apple2c/apple2c0/3420033a.256"], 0x8000, "Apple //c, ROM 0 (UniDisk 3.5)"),
    ("gb_rom_apple2c3", ["apple2c/apple2c3/342-0445-a.256"], 0x8000, "Apple //c, ROM 3 (extension mémoire)"),
    ("gb_rom_apple2c4", ["apple2c/apple2c4/3410445b.256"], 0x8000, "Apple //c, ROM 4"),
    ("gb_rom_apple2cp", ["apple2c/apple2cp/341-0625-a.256"], 0x8000, "Apple //c Plus (ROM 5)"),
    ("gb_rom_disk2", ["341-0027-a.p5"], 0x100, "Carte Disk II 16 secteurs (PROM P5 341-0027)"),
    ("gb_rom_disk2_p6", ["341-0028-a.rom"], 0x100, "Carte Disk II : séquenceur (PROM P6 341-0028)"),
    ("gb_rom_mouse", ["341-0270-c.4b"], 0x800, "Carte souris AppleMouse II (341-0270-C), 8 pages de 256 octets"),
    ("gb_rom_video2e", ["Apple IIe Video ROM - 342-0133-A - US 1982.bin"], 0x1000,
     "Générateur de caractères //e (US)"),
    ("gb_rom_video2e_enh", ["Apple IIe Enhanced Video ROM - 342-0265-A - US 1983.bin"], 0x1000,
     "Générateur de caractères //e Enhanced (US, MouseText)"),
    # ROM de 8 Ko : le jeu français est dans la première moitié, l'américain dans la seconde
    ("gb_rom_video2e_fr", ["apple2e/apple2efr/341-0163-a.e9"], 0x2000, "Générateur de caractères //e (français)"),
    ("gb_rom_video2e_enh_fr", ["apple2c/apple2c0fr/342-0274-a.e9"], 0x2000,
     "Générateur de caractères //e Enhanced et //c (français, MouseText)"),
]


def load_rom(names: list[str], size: int) -> bytes:
    # "vide:N" : N octets à $FF, un support de ROM resté vide
    data = b"".join(bytes([0xFF]) * int(n[5:]) if n.startswith("vide:") else (ROM_DIR / n).read_bytes() for n in names)
    if len(data) != size:
        raise SystemExit(f"[!] {' + '.join(names)} : {len(data)} octets, {size} attendus")
    # ROM de caractères : seuls les 2 premiers Ko portent les glyphes (256 x 8 lignes)
    return data[:0x800] if "Video ROM" in names[0] or names[0].endswith(".e9") else data


def to_c_array(data: bytes, name: str) -> str:
    lines = [f"const unsigned char {name}[{len(data)}]={{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append(",".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    blocks = []
    for array, names, size, comment in ROMS:
        blocks.append(f"// {comment} — {' + '.join(names)}\n{to_c_array(load_rom(names, size), array)}\n")

    header = f"""/*

Apple2_ProjectESP32 — ROM Apple II (moniteur, BASIC, Disk II, caractères)

Généré par scripts/rom2header.py

Ces ROM restent la propriété d'Apple.

*/

#ifndef ROMS_APPLE2_H
#define ROMS_APPLE2_H

{chr(10).join(blocks)}
#endif // ROMS_APPLE2_H
"""
    OUT.write_text(header, encoding="utf-8")
    print(f"[+] {OUT} : {len(ROMS)} ROM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
