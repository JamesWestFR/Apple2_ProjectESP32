"""Capture les logs série d'un ESP32 (ou autre périphérique série).

Usage :
    python read_serial.py [port] [baud] [secondes] [--no-reset]

Par défaut, pulse EN (RTS) pour resetter la carte et capturer le boot complet.
"""
import sys
import time


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0
    do_reset = "--no-reset" not in sys.argv

    import serial  # pyserial (fourni avec platformio/esptool)

    # Sortie redirigée sous Windows : l'encodage par défaut refuse certains caractères
    sys.stdout.reconfigure(errors="replace")

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.open()

    print(f"=== Ecoute sur {port} @ {baud} pendant {duration:.0f}s ===", flush=True)

    if do_reset:
        # Reset style esptool : IO0 haut (boot normal), EN bas puis haut
        ser.setDTR(False)  # IO0 = HIGH -> boot normal (pas mode flash)
        ser.setRTS(True)   # EN = LOW  -> reset
        time.sleep(0.1)
        ser.setRTS(False)  # EN = HIGH -> run
        print("=== Reset pulse envoye, capture du boot ===", flush=True)

    deadline = time.time() + duration
    captured = 0
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            captured += len(data)
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()

    ser.close()
    print(f"\n=== Capture terminee : {captured} octets ===", flush=True)


if __name__ == "__main__":
    main()
