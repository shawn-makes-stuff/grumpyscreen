#!/bin/bash
# GrumpyScreen desktop simulator: real UI in an SDL window + mock Moonraker.
# No printer, no docker. See SIMULATOR.md.
#
#   ./sim.sh                 # 480x272 window, 4 AFC lanes, port 7125
#   SIM_BACKEND=hh ./sim.sh  # Happy Hare mock instead of AFC
#   SIM_LANES=8 SIM_PORT=7127 SIM_RES=800x480 ./sim.sh
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${SIM_PORT:-7125}"
LANES="${SIM_LANES:-4}"
BACKEND="${SIM_BACKEND:-afc}"
RES="${SIM_RES:-}"

cd "$DIR"

# --- dependency check ---------------------------------------------------
for c in g++ make pkg-config python3 git; do
    command -v "$c" >/dev/null || { echo "ERROR: missing '$c'"; exit 1; }
done
pkg-config --exists sdl2 || { echo "ERROR: SDL2 dev package missing (apt install libsdl2-dev)"; exit 1; }
python3 -c 'import websockets' 2>/dev/null || { echo "ERROR: python websockets missing (apt install python3-websockets or pip install websockets)"; exit 1; }

# --- submodule patches (idempotent) --------------------------------------
for p in patches/*.patch; do
    [ -e "$p" ] || continue
    sub=$(basename "$p" | cut -d- -f1)   # lvgl-foo.patch -> lvgl
    [ -d "$sub" ] || { echo "WARN: no submodule dir '$sub' for $p"; continue; }
    if git -C "$sub" apply --reverse --check "$DIR/$p" 2>/dev/null; then
        echo "[patch] $p already applied"
    else
        git -C "$sub" apply "$DIR/$p"
        echo "[patch] applied $p"
    fi
done

# --- build ----------------------------------------------------------------
REV=$(git rev-parse --short HEAD 2>/dev/null || echo dev)
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo dev)
MAKE_ARGS="GUPPY_SDL=1 GUPPY_SMALL_SCREEN=true GUPPYSCREEN_VERSION=$REV GUPPYSCREEN_BRANCH=$BRANCH"
[ -n "$RES" ] && MAKE_ARGS="$MAKE_ARGS SDL_RES=$RES"

echo "[build] make $MAKE_ARGS"
make -j"$(nproc)" $MAKE_ARGS

# --- config ----------------------------------------------------------------
SIM_CFG=build/bin/grumpyscreen-sim.cfg
sed -e "s/^port:.*/port: $PORT/" grumpyscreen.cfg > "$SIM_CFG"

# --- run ---------------------------------------------------------------------
cleanup() { [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

echo "[mock] starting mock moonraker: backend=$BACKEND lanes=$LANES port=$PORT"
python3 -u tests/mock_moonraker.py --port "$PORT" --lanes "$LANES" --backend "$BACKEND" &
MOCK_PID=$!

# wait for the websocket port before launching the UI (it sticks on the
# "waiting for klipper" init panel if it connects before the mock is up)
for _ in $(seq 1 50); do
    python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), 0.2).close()" 2>/dev/null && break
    sleep 0.1
done

echo "[ui] launching grumpyscreen (close window or Ctrl-C to stop)"
CONFIG_FILE="$SIM_CFG" ./build/bin/grumpyscreen
