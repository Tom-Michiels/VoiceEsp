#!/usr/bin/env bash
# Genereert de MicroPython embed-port en zet hem als Arduino-library neer in
# ~/Documents/Arduino/libraries/MicroPythonEmbed. Idempotent: gewoon opnieuw
# draaien na een wijziging in tools/mpconfigport.h.
#
#   ./tools/build_micropython_lib.sh [pad-naar-micropython-checkout]
#
# Zonder argument wordt de repo (shallow) naar ~/.cache/micropython-src
# gekloond. Vereist: git, make, python3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MPTOP="${1:-$HOME/.cache/micropython-src}"
LIB="$HOME/Documents/Arduino/libraries/MicroPythonEmbed"

if [ ! -d "$MPTOP/py" ]; then
  echo "== micropython klonen naar $MPTOP"
  git clone --depth 1 https://github.com/micropython/micropython.git "$MPTOP"
fi

GEN=$(mktemp -d)
trap 'rm -rf "$GEN"' EXIT

cat > "$GEN/micropython_embed.mk" <<EOF
MICROPYTHON_TOP = $MPTOP
include \$(MICROPYTHON_TOP)/ports/embed/embed.mk
EOF
cp "$ROOT/tools/mpconfigport.h" "$GEN/mpconfigport.h"

echo "== embed-boom genereren"
( cd "$GEN" && make -f micropython_embed.mk ) | tail -2

echo "== library samenstellen in $LIB"
rm -rf "$LIB"
mkdir -p "$LIB/src"
cp -R "$GEN/micropython_embed/"* "$LIB/src/"
cp "$ROOT/tools/mpconfigport.h" "$LIB/src/"

# Patch 1: mphalport.c print naar printf/UART0; de sketch (embed_api.c)
# levert mp_hal_stdout_tx_strn_cooked zelf en stuurt naar USB-CDC.
rm "$LIB/src/port/mphalport.c"

# Patch 2: embed_util.c definieert __assert_func, maar newlib op de ESP32
# heeft die al - dubbele definitie bij het linken.
sed -i '' '/#ifndef NDEBUG/,/#endif/d' "$LIB/src/port/embed_util.c"

cat > "$LIB/src/micropython_embed.h" <<'EOF'
// Wrapper zodat Arduino de library herkent aan een header in de src-root.
#pragma once
#include "port/micropython_embed.h"
EOF

cat > "$LIB/library.properties" <<'EOF'
name=MicroPythonEmbed
version=1.0.0
author=MicroPython (embed port), gegenereerd voor EspMicTest
maintainer=Tom
sentence=MicroPython embed port als statische library voor de VoiceEsp-sketch.
paragraph=Gegenereerd met VoiceEsp/tools/build_micropython_lib.sh; niet met de hand aanpassen.
category=Other
url=https://github.com/micropython/micropython
architectures=esp32
EOF

echo "== klaar: $(find "$LIB/src" -name '*.c' | wc -l | tr -d ' ') C-bestanden"
