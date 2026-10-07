#!/usr/bin/env python3
"""
rom2header.py — Convertit les ROM Apple II de RomsApple/ en header C.

Génère Apple2_ProjectESP32/include/roms_apple2.h :
  - gb_rom_apple2        Apple ][ (Integer BASIC), 12 Ko, $D000-$FFFF
  - gb_rom_apple2plus    Apple ][+ (Applesoft, Autostart), 12 Ko, $D000-$FFFF
  - gb_rom_apple2e       Apple //e, 16 Ko, $C000-$FFFF
  - gb_rom_apple2e_enh   Apple //e Enhanced, 16 Ko, $C000-$FFFF
  - gb_rom_disk2         carte Disk II 16 secteurs (P5), 256 octets, $Cn00
  - gb_rom_video2e       générateur de caractères du //e, 2 Ko
  - gb_rom_video2e_enh   générateur de caractères du //e Enhanced (MouseText), 2 Ko

Usage (sans argument) : python scripts/rom2header.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_DIR = ROOT / "RomsApple"
OUT = ROOT / "Apple2_ProjectESP32" / "include" / "roms_apple2.h"

# (nom du tableau C, [fichiers concaténés], taille attendue, commentaire)
ROMS = [
    ("gb_rom_apple2", ["AppleWin - Apple2.rom"], 0x3000, "Apple ][ : Integer BASIC + moniteur, $D000-$FFFF"),
    ("gb_rom_apple2plus", ["AppleWin - Apple2_Plus.rom"], 0x3000, "Apple ][+ : Applesoft + Autostart, $D000-$FFFF"),
    ("gb_rom_apple2e",
     ["Apple IIe ROM Pages C0-DF - 342-0135-A - 1982.bin", "Apple IIe ROM Pages E0-FF - 342-0134-A - 1982.bin"],
     0x4000, "Apple //e, $C000-$FFFF"),
    ("gb_rom_apple2e_enh", ["Apple IIe Enhanced ROM Pages C0-FF - 342-0349-B - 1985.bin"],
     0x4000, "Apple //e Enhanced, $C000-$FFFF"),
    ("gb_rom_disk2", ["AppleWin - DISK2.rom"], 0x100, "Carte Disk II 16 secteurs (PROM P5 341-0027)"),
    ("gb_rom_video2e", ["Apple IIe Video ROM - 342-0133-A - US 1982.bin"], 0x1000,
     "Générateur de caractères //e (US)"),
    ("gb_rom_video2e_enh", ["Apple IIe Enhanced Video ROM - 342-0265-A - US 1983.bin"], 0x1000,
     "Générateur de caractères //e Enhanced (US, MouseText)"),
]


def load_rom(names: list[str], size: int) -> bytes:
    data = b"".join((ROM_DIR / n).read_bytes() for n in names)
    if len(data) != size:
        raise SystemExit(f"[!] {' + '.join(names)} : {len(data)} octets, {size} attendus")
    # ROM de caractères : seuls les 2 premiers Ko portent les glyphes (256 x 8 lignes)
    return data[:0x800] if "Video ROM" in names[0] else data


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
