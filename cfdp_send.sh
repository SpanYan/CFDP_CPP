#!/bin/bash
# Usage:
#   ./cfdp_send.sh [<device>]
#
# Sans device  → socat PTY + receiver_proc lancés automatiquement
# Avec device  → device UART fourni
#
# Génère un fichier de taille aléatoire (1–20 MB) avec contenu random.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
RECEIVER="$BUILD/receiver_proc"
SENDER="$ROOT/sender/sender.py"

FILE_SIZE_MB=200

TTY_GND=/tmp/tty_ground
TTY_SAT=/tmp/tty_sat
STATE_DIR=/tmp/cfdp_state
RECV_DIR=/tmp/cfdp_recv

# ─── Arguments ────────────────────────────────────────────────────────────────

usage() {
    echo "Usage: $0 [<device>]"
    exit 1
}

case $# in
    0)  DEVICE="" ;;
    1)  DEVICE="$1" ;;
    *)  usage ;;
esac

# ─── Génération du fichier source ─────────────────────────────────────────────

FILEPATH=/tmp/cfdp_src.bin
DEST=cfdp_src.bin

if [ -f "$STATE_DIR/state.bin" ] && [ -f "$FILEPATH" ]; then
    echo "[recovery] journal receiver existant → réutilisation de $FILEPATH"
else
    echo "[gen] fichier aléatoire : ${FILE_SIZE_MB} MB → $FILEPATH"
    dd if=/dev/urandom of="$FILEPATH" bs=1M count="$FILE_SIZE_MB" 2>/dev/null
    rm -f "$STATE_DIR/state.bin"
fi

# ─── Build receiver_proc si nécessaire ────────────────────────────────────────

echo "[build] compilation receiver_proc..."
mkdir -p "$BUILD"
[ -f "$BUILD/CMakeCache.txt" ] || \
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target receiver_proc -j"$(nproc)"

# ─── Répertoires de travail ───────────────────────────────────────────────────

mkdir -p "$STATE_DIR" "$RECV_DIR"

# ─── PTY virtuels (si pas de device fourni) ───────────────────────────────────

SOCAT_PID=""
RECV_PID=""

cleanup() {
    [ -n "$RECV_PID"  ] && kill "$RECV_PID"  2>/dev/null || true
    [ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

if [ -z "$DEVICE" ]; then
    rm -f "$TTY_GND" "$TTY_SAT"
    socat -d -d \
        "pty,raw,echo=0,link=$TTY_GND" \
        "pty,raw,echo=0,link=$TTY_SAT" \
        2>/tmp/socat.log &
    SOCAT_PID=$!

    until [ -e "$TTY_GND" ] && [ -e "$TTY_SAT" ]; do sleep 0.05; done
    echo "[setup] PTY : $TTY_GND ↔ $TTY_SAT"
    DEVICE="$TTY_GND"
fi

# ─── Receiver ────────────────────────────────────────────────────────────────

"$RECEIVER" "$TTY_SAT" "$STATE_DIR" "$RECV_DIR" &
RECV_PID=$!
sleep 0.1

# ─── Sender Python ────────────────────────────────────────────────────────────

set +e
python3 "$SENDER" "$DEVICE" "$FILEPATH" "$DEST"
SENDER_EXIT=$?
set -e

wait "$RECV_PID" 2>/dev/null || true
RECV_EXIT=${PIPESTATUS[0]:-0}

# ─── Résultat ─────────────────────────────────────────────────────────────────

echo ""
if [ $SENDER_EXIT -ne 0 ]; then
    echo "ÉCHEC sender (exit $SENDER_EXIT)"
    exit 1
fi

RECEIVED="$RECV_DIR/$DEST"
if [ ! -f "$RECEIVED" ]; then
    echo "ÉCHEC : fichier non reçu dans $RECV_DIR"
    exit 1
fi

if cmp -s "$FILEPATH" "$RECEIVED"; then
    echo "SUCCÈS : $RECEIVED identique à $FILEPATH ✓"
else
    echo "ÉCHEC : fichiers différents"
    exit 1
fi
