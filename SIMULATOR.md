# GrumpyScreen Simulator

Run the real GrumpyScreen UI in a desktop window against a mock Moonraker —
no printer, no docker. Good for testing panels (AFC, Happy Hare) interactively.

## Setup (Debian/Ubuntu, incl. WSLg)

```
sudo apt install build-essential pkg-config libsdl2-dev python3-websockets
git clone --recursive -b afc-panel https://github.com/shawn-makes-stuff/grumpyscreen
cd grumpyscreen
./sim.sh
```

First build takes a few minutes (LVGL + libhv + wpa_supplicant); rebuilds are
incremental. A 480x272 window opens, connected to a fake 4-lane AFC. Mouse =
touch. Load/unload/tool-change interactions mutate the mock state and push
live updates back to the UI.

## Options (env vars)

| Var | Default | Meaning |
|-----|---------|---------|
| `SIM_BACKEND` | `afc` | `afc` = AFC lane objects, `hh` = Happy Hare `mmu` object |
| `SIM_LANES` | `4` | number of lanes/gates |
| `SIM_PORT` | `7125` | mock Moonraker websocket port (change if a real Moonraker runs locally) |
| `SIM_RES` | `480x272` | SDL window size, e.g. `800x480` (triggers rebuild) |

Example: `SIM_BACKEND=hh SIM_LANES=8 ./sim.sh`

## Pieces

- `sim.sh` — builds (`make GUPPY_SDL=1 GUPPY_SMALL_SCREEN=true`), starts the
  mock, launches the UI, cleans up on exit.
- `tests/mock_moonraker.py` — standalone fake Moonraker websocket server.
  Fakes `printer.info`/`server.info`/`printer.objects.*`, AFC or Happy Hare
  state, and handles the gcode commands the panels send (`TOOL_LOAD`,
  `CHANGE_TOOL`, `MMU_CHANGE_TOOL`, ...) so interactions round-trip.
- `patches/` — two small submodule fixes `sim.sh` applies automatically:
  - `lvgl-colorwheel.patch`: closes hairline gaps on large color wheels.
  - `lv_drivers-wayland-xdg.patch`: compile fix for xdg-shell v5 (only
    matters for `GUPPY_WAYLAND` builds; harmless otherwise).

## Notes

- The SDL target is plumbed in `Makefile` (`GUPPY_SDL`), `lv_drv_conf.h`, and
  `src/main.cpp` — the driver itself is the stock lv_drivers SDL driver.
- The UI binary is byte-for-byte the same panel/websocket code that runs on a
  printer; only the display/input driver differs.
- Mock state is in-memory and resets on restart.
