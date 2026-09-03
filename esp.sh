#!/usr/bin/env bash
# Helper voor de ESP32-C3 SuperMini + INMP441 test-sketch.
#   ./esp.sh build              compileren
#   ./esp.sh upload [poort]     compileren + flashen
#   ./esp.sh monitor [poort]    seriele monitor (ctrl-C om te stoppen)
#   ./esp.sh record [poort]     3 s opnemen naar recording-esp.wav
#   ./esp.sh port               gedetecteerde poort tonen
set -euo pipefail

# huge_app: 3 MB app-partitie (geen OTA) - nodig sinds MicroPython meegelinkt wordt
FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app"
ROOT="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$ROOT/EspMicTest"

# arduino-cli staat niet in PATH; de Arduino IDE heeft er een meegeleverd.
BUNDLED="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
if command -v arduino-cli >/dev/null 2>&1; then
  ACLI=$(command -v arduino-cli)
elif [ -x "$BUNDLED" ]; then
  ACLI="$BUNDLED"
else
  echo "arduino-cli niet gevonden (ook niet in de Arduino IDE)." >&2
  exit 1
fi

detect_port() {
  local p
  p=$("$ACLI" board list --format json 2>/dev/null \
      | python3 -c 'import json,sys
d = json.load(sys.stdin)
for p in d.get("detected_ports", []):
    for b in p.get("matching_boards", []) or []:
        if "ESP32" in b.get("name", ""):
            print(p["port"]["address"]); raise SystemExit
' || true)
  [ -n "$p" ] || p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -n "$p" ] || { echo "Geen ESP32 gevonden - hangt hij aan de USB?" >&2; exit 1; }
  echo "$p"
}

cmd=${1:-build}
case "$cmd" in
  build)   "$ACLI" compile -b "$FQBN" "$SKETCH" ;;
  upload)  PORT=${2:-$(detect_port)}
           "$ACLI" compile -b "$FQBN" "$SKETCH"
           "$ACLI" upload -b "$FQBN" -p "$PORT" "$SKETCH" ;;
  monitor) PORT=${2:-$(detect_port)}
           "$ACLI" monitor -p "$PORT" -c baudrate=115200 ;;
  record)  PORT=${2:-$(detect_port)}
           "$ROOT/.venv/bin/python" "$ROOT/tools/record_wav.py" -p "$PORT" -o "$ROOT/recording-esp.wav" ;;
  port)    detect_port ;;
  *)       sed -n '2,8p' "$0"; exit 1 ;;
esac
