#!/usr/bin/env python3
"""
Stuurt 'w' naar de XIAO nRF52840 Sense, vangt de base64-audiodump op en
schrijft er een WAV-bestand van. Print daarna wat statistieken zodat je
meteen ziet of de microfoon echt geluid opvangt.

    python3 tools/record_wav.py                  # auto-detect poort
    python3 tools/record_wav.py -p /dev/cu.usbmodem1101 -o test.wav
"""

import argparse
import base64
import glob
import math
import struct
import sys
import time
import wave

try:
    import serial
except ImportError:
    sys.exit("pyserial ontbreekt. Installeer met: .venv/bin/pip install pyserial")


def find_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if not candidates:
        sys.exit("Geen seriële poort gevonden. Hangt het bord aan de USB? Geef anders -p op.")
    return candidates[0]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="seriële poort (default: auto)")
    ap.add_argument("-o", "--out", default="recording.wav", help="uitvoerbestand")
    ap.add_argument("-t", "--timeout", type=float, default=20.0, help="max wachttijd in seconden")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Poort   : {port}")

    with serial.Serial(port, 115200, timeout=1) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"m")      # monitor uit, anders lopen de regels door de dump heen
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"w")      # opname starten

        chunks: list[str] = []
        header: dict[str, str] = {}
        started = False
        deadline = time.time() + args.timeout

        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("ascii", "replace").strip()
            if line.startswith("#WAV-BEGIN"):
                header = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
                print(f"Header  : {header}")
                started = True
                continue
            if line.startswith("#WAV-END"):
                print(f"Einde   : {line}")
                break
            if started and line:
                chunks.append(line)
        else:
            print("!! Timeout tijdens opname", file=sys.stderr)

        ser.write(b"m")      # monitor weer aan

    if not chunks:
        sys.exit("Geen audiodata ontvangen.")

    pcm = base64.b64decode("".join(chunks))
    rate = int(header.get("rate", 16000))
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm[: len(pcm) // 2 * 2])

    with wave.open(args.out, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm)

    n = len(samples)
    dc = sum(samples) / n
    rms = math.sqrt(sum((s - dc) ** 2 for s in samples) / n)
    peak = max(abs(s) for s in samples)
    dbfs = 20 * math.log10(rms / 32768) if rms > 0 else -120.0
    clipped = sum(1 for s in samples if abs(s) >= 32000)

    print(f"Bestand : {args.out}")
    print(f"Samples : {n} ({n / rate:.2f} s @ {rate} Hz)")
    print(f"DC      : {dc:.0f}")
    print(f"RMS     : {rms:.0f}  ({dbfs:.1f} dBFS)")
    print(f"Peak    : {peak}  ({20 * math.log10(peak / 32768):.1f} dBFS)" if peak else "Peak    : 0")
    print(f"Clipping: {clipped} samples")

    if peak == 0:
        print("\n-> Alleen nullen: de microfoon levert niets aan. Controleer de PDM-pinnen/gain.")
    elif dbfs < -70:
        print("\n-> Erg stil. Praat dichter bij het bord of verhoog de gain met '+' in de monitor.")
    elif clipped > n * 0.01:
        print("\n-> Veel clipping. Verlaag de gain met '-' in de monitor.")
    else:
        print("\n-> Ziet er gezond uit.")


if __name__ == "__main__":
    main()
