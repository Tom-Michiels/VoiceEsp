#!/usr/bin/env bash
# Helper voor de XIAO nRF52840 Sense test-sketch.
#   ./xiao.sh build              compileren
#   ./xiao.sh upload [poort]     compileren + flashen
#   ./xiao.sh monitor [poort]    seriële monitor (ctrl-C om te stoppen)
#   ./xiao.sh record [poort]     3 s opnemen naar recording.wav
#   ./xiao.sh port               gedetecteerde poort tonen
set -euo pipefail

FQBN="Seeeduino:mbed:xiaonRF52840Sense"
SKETCH="$(cd "$(dirname "$0")" && pwd)/XiaoSenseTest"
ROOT="$(cd "$(dirname "$0")" && pwd)"

detect_port() {
  local p
  p=$(arduino-cli board list --format json 2>/dev/null \
      | python3 -c 'import json,sys
d=json.load(sys.stdin)
for p in d.get("detected_ports", []):
    for b in p.get("matching_boards", []) or []:
        if "nRF52840" in b.get("name",""):
            print(p["port"]["address"]); raise SystemExit
' || true)
  [ -n "$p" ] || p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -n "$p" ] || { echo "Geen XIAO gevonden - hangt hij aan de USB?" >&2; exit 1; }
  echo "$p"
}

cmd=${1:-build}
case "$cmd" in
  build)   arduino-cli compile -b "$FQBN" "$SKETCH" ;;
  upload)  PORT=${2:-$(detect_port)}
           arduino-cli compile -b "$FQBN" "$SKETCH"
           arduino-cli upload -b "$FQBN" -p "$PORT" "$SKETCH" ;;
  monitor) PORT=${2:-$(detect_port)}
           arduino-cli monitor -p "$PORT" -c baudrate=115200 ;;
  record)  PORT=${2:-$(detect_port)}
           "$ROOT/.venv/bin/python" "$ROOT/tools/record_wav.py" -p "$PORT" -o "$ROOT/recording.wav" ;;
  port)    detect_port ;;
  *)       sed -n '2,8p' "$0"; exit 1 ;;
esac
