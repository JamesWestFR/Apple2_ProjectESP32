"""Envoie des commandes à la console série d'Apple2_ProjectESP32 et affiche les réponses.

Usage :
    python a2console.py [--port COM5] [--reset] [--wait S] commande [commande ...]

Chaque commande est une ligne de la console du firmware (voir
Apple2_ProjectESP32/src/SerialConsole.cpp) : s, t, f, "k TEXTE", "d1 CHEMIN", b, r...
S'y ajoutent trois commandes traitées par ce script :
    "wait N"                      attend N secondes sans rien envoyer
    "put FICHIER_LOCAL=/sd/NOM"   copie un fichier du PC sur la carte SD
    "putbig FICHIER_LOCAL=/sd/NOM"  de même pour un grand fichier (binaire, 921 600 bauds)
    "shot FICHIER.png"            rapatrie l'image affichée par la carte

Exemple :
    python a2console.py b "wait 3" r "wait 1" "k PRINT 6*7\\r" "wait 1" t s
"""
import struct
import sys
import time
import zlib


def main() -> None:
    args = sys.argv[1:]
    port, reset, wait = "COM5", False, 1.0
    commands = []
    i = 0
    while i < len(args):
        if args[i] == "--port":
            port = args[i + 1]
            i += 2
        elif args[i] == "--wait":
            wait = float(args[i + 1])
            i += 2
        elif args[i] == "--reset":
            reset = True
            i += 1
        else:
            commands.append(args[i])
            i += 1

    import serial  # pyserial (fourni avec platformio/esptool)

    sys.stdout.reconfigure(errors="replace")

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.1
    # Ouvrir le port sans toucher à DTR ni RTS : sinon la carte redémarre
    ser.dtr = False
    ser.rts = False
    ser.open()

    def pump(seconds: float) -> None:
        deadline = time.time() + seconds
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace").replace("\r", ""))
                sys.stdout.flush()

    def put(local: str, remote: str) -> None:
        """Copie un fichier du PC sur la carte SD, en hexadécimal."""
        data = open(local, "rb").read()
        print(f">>> put {len(data)} {remote}", flush=True)
        ser.write(f"put {len(data)} {remote}\n".encode("ascii"))
        ser.flush()
        pump(0.5)
        start = time.time()
        for pos in range(0, len(data), 32):
            ser.write(data[pos:pos + 32].hex().encode("ascii") + b"\n")
        ser.flush()
        pump(3.0)
        print(f"({len(data)} octets, somme {sum(data)}, {time.time() - start:.0f} s)", flush=True)

    def putbig(local: str, remote: str, baud: int = 921600) -> None:
        """Copie un grand fichier sur la carte SD : binaire, par blocs de 4 Ko vérifiés."""
        data = open(local, "rb").read()
        print(f">>> putbig {len(data)} {baud} {remote}", flush=True)
        ser.reset_input_buffer()
        ser.write(f"putbig {len(data)} {baud} {remote}\n".encode("ascii"))
        ser.flush()
        answer = b""
        deadline = time.time() + 5
        while time.time() < deadline and b"ready" not in answer and b"FAILED" not in answer:
            answer += ser.read(256)
        if b"ready" not in answer:
            print(answer.decode("ascii", errors="replace"))
            return
        time.sleep(0.2)
        ser.baudrate = baud
        ser.timeout = 5
        time.sleep(0.3)
        ser.reset_input_buffer()
        start, pos, retries = time.time(), 0, 0
        last_report = 0
        while pos < len(data):
            block = data[pos:pos + 4096]
            ser.write(bytes((0xA5,)) + block + struct.pack("<I", sum(block)))
            ser.flush()
            reply = ser.read(1)
            if reply == b"K":
                pos += len(block)
            elif reply == b"X" or retries > 60:
                print("(transfert abandonné)", flush=True)
                break
            else:
                retries += 1
            if pos - last_report >= 2 * 1024 * 1024:
                last_report = pos
                elapsed = time.time() - start
                print(f"  {pos >> 20} / {len(data) >> 20} Mo, {pos / 1024 / elapsed:.0f} Ko/s, {retries} renvois", flush=True)
        time.sleep(0.5)
        ser.baudrate = 115200
        ser.timeout = 0.1
        pump(3.0)
        print(f"({pos} octets en {time.time() - start:.0f} s, {retries} renvois)", flush=True)

    def shot(path: str) -> None:
        """Rapatrie le framebuffer VGA de la carte et l'écrit en PNG (560 x 384)."""
        print(f">>> shot {path}", flush=True)
        ser.write(b"p\n")
        ser.flush()
        rows, buf, deadline = {}, b"", time.time() + 60
        while time.time() < deadline and b"A2 fb end" not in buf:
            buf += ser.read(8192)
        for text in buf.decode("ascii", errors="replace").splitlines():
            parts = text.strip().split(" ")
            if len(parts) == 4 and parts[0] == "A2" and parts[1] == "fb" and parts[2].isdigit():
                try:
                    rows[int(parts[2])] = bytes.fromhex(parts[3])
                except ValueError:
                    pass
        if not rows:
            print("(aucune ligne reçue)")
            return
        width = max(len(r) for r in rows.values())
        height = max(rows) + 1
        raw = bytearray()
        for y in range(height * 2):
            raw.append(0)
            row = rows.get(y // 2, b"").ljust(width, bytes(1))
            for c in row:
                # 2 bits par composante : R en bits 0-1, V en 2-3, B en 4-5
                raw += bytes(((c & 3) * 85, ((c >> 2) & 3) * 85, ((c >> 4) & 3) * 85))

        def chunk(kind: bytes, body: bytes) -> bytes:
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

        signature = bytes((0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
        png = signature + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height * 2, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
        open(path, "wb").write(png)
        print(f"({len(rows)} lignes de {width} points -> {path})", flush=True)

    if reset:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        pump(4.0)

    for command in commands:
        if command.startswith("wait "):
            pump(float(command[5:]))
            continue
        if command.startswith("shot "):
            shot(command[5:])
            continue
        if command.startswith("putbig "):
            # putbig FICHIER_LOCAL=CHEMIN_SUR_LA_CARTE
            local, _, remote = command[7:].rpartition("=")
            putbig(local, remote)
            continue
        if command.startswith("put "):
            # put FICHIER_LOCAL=CHEMIN_SUR_LA_CARTE
            local, _, remote = command[4:].rpartition("=")
            put(local, remote)
            continue
        print(f">>> {command}", flush=True)
        ser.write((command + "\n").encode("ascii", errors="replace"))
        ser.flush()
        pump(wait)

    ser.close()


if __name__ == "__main__":
    main()
