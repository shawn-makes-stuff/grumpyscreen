#!/usr/bin/env python3
"""Mock moonraker websocket server for testing grumpyscreen panels without a printer.

Serves a fake AFC (4-lane canvas) plus basic printer objects. AFC gcode
commands (TOOL_LOAD, CHANGE_TOOL, TOOL_UNLOAD, LANE_UNLOAD, RESET_FAILURE)
mutate the fake state and push notify_status_update, so panel interactions
are visible live.

Usage: python3 mock_moonraker.py [--port 7125]
"""
import argparse
import asyncio
import json
import time

import websockets


def make_lane(name, index, map_cmd, material="", color="", prep=False, load=False):
    return {
        "name": name,
        "unit": "CANVAS_1",
        "hub": "toolhead_4way_hub",
        "extruder": "extruder",
        "buffer": "",
        "buffer_status": "",
        "lane": index,
        # AFC >=1.2.x publishes this as a list of the T(n) macros
        "map": [t for t in map_cmd.split(",") if t] if isinstance(map_cmd, str) else map_cmd,
        "load": load,
        "prep": prep,
        "tool_loaded": False,
        "loaded_to_hub": False,
        "material": material,
        "spool_id": None,
        "color": color,
        "weight": 1000,
        "extruder_temp": None,
        "bed_temp": None,
        "runout_lane": None,
        "filament_status": "Ready" if (prep and load) else "Not Ready",
        "filament_status_led": "ready",
        "status": "",
        "dist_hub": 1200,
    }


STATE = {
    "extruder": {"temperature": 24.6, "target": 0, "power": 0.0, "can_extrude": False, "pressure_advance": 0.035, "smooth_time": 1.5},
    "heater_bed": {"temperature": 23.1, "target": 0, "power": 0.0},
    "temperature_fan chamber_fan": {"temperature": 25.0, "target": 0, "speed": 0.0},
    "temperature_sensor chamber_temp": {"temperature": 25.0},
    "print_stats": {"filename": "", "total_duration": 0.0, "print_duration": 0.0,
                    "filament_used": 0.0, "state": "standby", "message": "",
                    "info": {"total_layer": None, "current_layer": None}},
    "toolhead": {"homed_axes": "", "position": [0, 0, 0, 0], "max_velocity": 500},
    "gcode_move": {"speed_factor": 1.0, "extrude_factor": 1.0, "homing_origin": [0, 0, 0, 0]},
    "display_status": {"progress": 0.0, "message": ""},
    "fan": {"speed": 0.0},
    "AFC": {
        "version": "1.0.0-mock",
        "current_load": None,
        "current_lane": None,
        "next_lane": None,
        "current_state": "Idle",
        "current_toolchange": 0,
        "number_of_toolchanges": 0,
        "spoolman": True,
        "error_state": False,
        "bypass_state": False,
        "quiet_mode": False,
        "position_saved": False,
        "units": ["Canvas CANVAS_1"],
        "lanes": [],
        "maps": [],
        "extruders": ["extruder"],
        "hubs": ["toolhead_4way_hub"],
        "buffers": [],
        "message": {"message": "", "type": ""},
        "led_state": True,
    },
}

# 16 distinct spool presets (material, color, filled, map, weight).
# Empty slots are bare — no color/material until the user sets them manually.
LANE_PRESETS = [
    ("PLA", "#F44336", True, "T0", 950),
    ("PETG", "#2196F3", True, "T1", 800),
    ("ABS", "#4CAF50", True, "T2", 1000),
    ("", "", False, "", 0),
    ("PLA", "#FFEB3B", True, "T0", 1000),
    ("PETG", "#212121", True, "T1", 750),
    ("ABS", "#FFFFFF", True, "", 1000),
    ("", "", False, "", 0),
    ("PLA", "#9C27B0", True, "T3", 600),
    ("PETG", "#00BCD4", True, "", 900),
    ("", "", False, "", 0),
    ("PLA", "#795548", True, "T2", 450),
    ("ASA", "#3F51B5", True, "", 1000),
    ("", "", False, "", 0),
    ("PLA", "#E91E63", True, "T0", 300),
    ("PETG", "#8BC34A", True, "", 1000),
]


def build_lanes(count):
    count = max(1, min(count, len(LANE_PRESETS)))
    STATE["AFC"]["lanes"] = []
    STATE["AFC"]["maps"] = []
    for i in range(count):
        mat, col, filled, mp, wt = LANE_PRESETS[i]
        name = f"lane{i + 1}"
        STATE["AFC"]["lanes"].append(name)
        if mp:
            STATE["AFC"]["maps"].append(f"{name}:{mp}")
        lane_data = make_lane(
            name=name,
            index=i,
            map_cmd=mp,
            material=mat,
            color=col,
            prep=filled,
            load=filled,
        )
        lane_data["weight"] = wt
        STATE[f"AFC_lane {name}"] = lane_data
    # default load lane 1 if it has filament
    if count > 0 and LANE_PRESETS[0][2]:
        STATE["AFC"]["current_load"] = "lane1"
        STATE["AFC_lane lane1"]["tool_loaded"] = True
        STATE["AFC_lane lane1"]["loaded_to_hub"] = True
    # demo infinite spool: lane5 backs up lane1
    if count >= 5:
        STATE["AFC_lane lane1"]["runout_lane"] = "lane5"


CLIENTS = set()

BACKEND = "afc"  # or "hh" (Happy Hare: single "mmu" printer object)

# Happy Hare spoolman mode: off / readonly / push / pull. Only "pull" makes HH
# refuse local colour+material edits, which the panel reflects by greying out
# the spool metadata controls.
SPOOLMAN_MODE = "push"

# name -> normalized rgb, for the couple of w3c names the presets use
HH_COLOR_NAMES = {"indigo": (0.294, 0.0, 0.510), "red": (1.0, 0.0, 0.0)}


def hh_rgb(color):
    """gate_color entry -> gate_color_rgb tuple, like Happy Hare publishes."""
    if not color:
        return (0.0, 0.0, 0.0)
    if color in HH_COLOR_NAMES:
        return HH_COLOR_NAMES[color]
    h = color.lstrip("#")
    try:
        return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    except (ValueError, IndexError):
        return (0.0, 0.0, 0.0)


def build_hh(count):
    count = max(1, min(count, len(LANE_PRESETS)))
    del STATE["AFC"]
    colors, materials, status, spool_ids = [], [], [], []
    for i in range(count):
        mat, col, filled, _mp, _wt = LANE_PRESETS[i]
        materials.append(mat)
        colors.append(col.lstrip("#"))
        status.append(1 if filled else 0)
        spool_ids.append(i + 1 if filled else -1)
    if count >= 2:
        colors[1] = "indigo"  # exercise the w3c-name -> rgb path
    if count >= 6:
        status[5] = -1  # exercise the unknown-gate state
    STATE["mmu"] = {
        "enabled": True,
        "num_gates": count,
        "gate": 0 if status[0] > 0 else -1,
        "tool": 0 if status[0] > 0 else -1,
        "unit": 0,
        "last_tool": -1,
        "next_tool": -1,
        "filament": "Loaded" if status[0] > 0 else "Unloaded",
        "filament_pos": 10,
        "action": "Idle",
        "print_state": "ready",
        "gate_status": status,
        "gate_material": materials,
        "gate_color": colors,
        "gate_color_rgb": [hh_rgb(c) for c in colors],
        "gate_spool_id": spool_ids,
        "ttg_map": list(range(count)),
        "endless_spool_groups": list(range(count)),
        "endless_spool_enabled": 1,
        "spoolman_support": SPOOLMAN_MODE,
        "has_bypass": True,
    }


async def hh_push():
    await push({"mmu": STATE["mmu"]})


async def hh_do_load(gate, tool):
    mmu = STATE["mmu"]
    mmu["action"] = "Loading"
    await hh_push()
    await asyncio.sleep(1.5)
    mmu["gate"] = gate
    mmu["tool"] = tool
    mmu["filament"] = "Loaded"
    # HH knows filament is present once it has pulled from the gate
    mmu["gate_status"][gate] = 1
    mmu["action"] = "Idle"
    await hh_push()
    print(f"  -> gate {gate} loaded (T{tool})")


async def hh_do_unload():
    mmu = STATE["mmu"]
    mmu["action"] = "Unloading"
    await hh_push()
    await asyncio.sleep(1.5)
    mmu["filament"] = "Unloaded"
    # unloading parks the filament in the buffer, so HH promotes the gate
    # from 1 (on spool) to 2 (available from buffer) rather than emptying it
    gate = mmu["gate"]
    if 0 <= gate < mmu["num_gates"] and mmu["gate_status"][gate] != 0:
        mmu["gate_status"][gate] = 2
    mmu["action"] = "Idle"
    await hh_push()
    print("  -> tool unloaded")


async def handle_hh_gcode(cmd, params):
    mmu = STATE["mmu"]
    if cmd == "MMU_CHANGE_TOOL" and "TOOL" in params:
        tool = int(params["TOOL"])
        gate = mmu["ttg_map"][tool] if tool < len(mmu["ttg_map"]) else tool
        asyncio.create_task(hh_do_load(gate, tool))
    elif cmd == "MMU_SELECT" and "GATE" in params:
        mmu["gate"] = int(params["GATE"])
        await hh_push()
    elif cmd == "MMU_LOAD":
        gate = mmu["gate"]
        tools = [t for t, g in enumerate(mmu["ttg_map"]) if g == gate]
        asyncio.create_task(hh_do_load(gate, tools[0] if tools else -1))
    elif cmd == "MMU_UNLOAD":
        asyncio.create_task(hh_do_unload())
    elif cmd == "MMU_EJECT" and "GATE" in params:
        gate = int(params["GATE"])
        mmu["gate_status"][gate] = 0
        await hh_push()
        print(f"  -> gate {gate} ejected")
    elif cmd == "MMU_GATE_MAP" and "GATE" in params:
        gate = int(params["GATE"])
        if "MATERIAL" in params:
            mmu["gate_material"][gate] = params["MATERIAL"]
        if "COLOR" in params:
            mmu["gate_color"][gate] = params["COLOR"]
            mmu["gate_color_rgb"][gate] = hh_rgb(params["COLOR"])
        if "SPOOLID" in params:
            mmu["gate_spool_id"][gate] = int(params["SPOOLID"])
        await hh_push()
        print(f"  -> gate {gate} map updated")
    elif cmd == "MMU_ENDLESS_SPOOL" and "GROUPS" in params:
        mmu["endless_spool_groups"] = [int(x) for x in params["GROUPS"].split(",")]
        await hh_push()
        print(f"  -> endless spool groups {mmu['endless_spool_groups']}")
    elif cmd in ("MMU_RECOVER", "MMU_UNLOCK"):
        mmu["print_state"] = "ready"
        await hh_push()
        print(f"  -> {cmd.lower()}: print_state ready")
    elif cmd == "SIM_ERROR":
        # testbench-only: toggle a pause_locked failure
        locked = mmu["print_state"] != "pause_locked"
        mmu["print_state"] = "pause_locked" if locked else "ready"
        await hh_push()
        print(f"  -> print_state {mmu['print_state']}")
    elif cmd == "SIM_PRINT":
        ps = STATE["print_stats"]
        ps["state"] = "printing" if ps["state"] != "printing" else "standby"
        await push({"print_stats": ps})
    return "ok"


def lane_key(lane_name):
    return f"AFC_lane {lane_name}"


async def push(payload):
    msg = json.dumps({
        "jsonrpc": "2.0",
        "method": "notify_status_update",
        "params": [payload, time.monotonic()],
    })
    for c in list(CLIENTS):
        try:
            await c.send(msg)
        except Exception:
            pass


async def do_load(lane_name, toolchange=False):
    afc = STATE["AFC"]
    afc["current_state"] = "Tool Changing" if toolchange else "Loading"
    afc["current_lane"] = lane_name
    await push({"AFC": afc})
    await asyncio.sleep(1.5)

    if afc["current_load"]:
        old = STATE[lane_key(afc["current_load"])]
        old["tool_loaded"] = False
        await push({lane_key(afc["current_load"]): old})

    lane = STATE[lane_key(lane_name)]
    lane["prep"] = True
    lane["load"] = True
    lane["tool_loaded"] = True
    lane["loaded_to_hub"] = True
    afc["current_load"] = lane_name
    afc["current_lane"] = None
    afc["current_state"] = "Idle"
    await push({"AFC": afc, lane_key(lane_name): lane})
    print(f"  -> {lane_name} loaded")


async def do_unload():
    afc = STATE["AFC"]
    if not afc["current_load"]:
        return
    afc["current_state"] = "Unloading"
    await push({"AFC": afc})
    await asyncio.sleep(1.5)
    lane = STATE[lane_key(afc["current_load"])]
    lane["tool_loaded"] = False
    key = lane_key(afc["current_load"])
    afc["current_load"] = None
    afc["current_state"] = "Idle"
    await push({"AFC": afc, key: lane})
    print("  -> tool unloaded")


async def do_eject(lane_name):
    lane = STATE[lane_key(lane_name)]
    lane["prep"] = False
    lane["load"] = False
    lane["loaded_to_hub"] = False
    lane["tool_loaded"] = False
    lane["filament_status"] = "Not Ready"
    # AFC clears the spool info when a lane is ejected
    lane["color"] = ""
    lane["material"] = ""
    lane["weight"] = 0
    lane["spool_id"] = None
    afc = STATE["AFC"]
    if afc.get("current_load") == lane_name:
        afc["current_load"] = None
    key = lane_key(lane_name)
    await push({key: lane, "AFC": afc})
    print(f"  -> {lane_name} ejected (prep=False, load=False)")


async def handle_gcode(script):
    print(f"gcode: {script}")
    # the bridge may send multi-line scripts (MMU_SELECT + MMU_LOAD)
    lines = [l for l in script.splitlines() if l.strip()]
    if len(lines) > 1:
        for line in lines:
            await handle_gcode(line)
        return "ok"

    parts = script.split()
    cmd = parts[0].upper() if parts else ""
    params = {}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            params[k.upper()] = v.strip('"').strip("'")

    if BACKEND == "hh":
        return await handle_hh_gcode(cmd, params)

    if cmd in ("TOOL_LOAD", "CHANGE_TOOL") and "LANE" in params:
        asyncio.create_task(do_load(params["LANE"], toolchange=cmd == "CHANGE_TOOL"))
    elif cmd == "TOOL_UNLOAD":
        asyncio.create_task(do_unload())
    elif cmd == "LANE_UNLOAD" and "LANE" in params:
        asyncio.create_task(do_eject(params["LANE"]))
    elif cmd == "SET_MATERIAL" and "LANE" in params:
        lane = STATE[lane_key(params["LANE"])]
        lane["material"] = params.get("MATERIAL", "")
        await push({lane_key(params["LANE"]): lane})
    elif cmd == "SET_COLOR" and "LANE" in params:
        lane = STATE[lane_key(params["LANE"])]
        lane["color"] = "#" + params.get("COLOR", "").lstrip("#")
        await push({lane_key(params["LANE"]): lane})
    elif cmd == "SET_RUNOUT" and "LANE" in params:
        lane = STATE[lane_key(params["LANE"])]
        runout = params.get("RUNOUT", "NONE")
        lane["runout_lane"] = None if runout.upper() == "NONE" else runout
        await push({lane_key(params["LANE"]): lane})
        print(f"  -> {params['LANE']} runout set to '{runout}'")
    elif cmd == "SET_MAP" and "LANE" in params:
        lane = STATE[lane_key(params["LANE"])]
        map_val = params.get("MAP", "NONE")
        if map_val.upper() in ("NONE", '""', "''"):
            map_val = ""
        lane["map"] = map_val
        await push({lane_key(params["LANE"]): lane})
        print(f"  -> {params['LANE']} map set to '{map_val}'")
    elif cmd in ("AFC_BYPASS", "SET_BYPASS"):
        afc = STATE["AFC"]
        afc["bypass_state"] = not afc.get("bypass_state", False)
        await push({"AFC": afc})
        print(f"  -> AFC bypass set to {afc['bypass_state']}")
    elif cmd == "RESET_FAILURE":
        afc = STATE["AFC"]
        afc["error_state"] = False
        afc["message"] = {"message": "", "type": ""}
        await push({"AFC": afc})
    elif cmd == "SIM_PRINT":
        # testbench-only: toggle a fake print so the print status screen shows.
        ps = STATE["print_stats"]
        printing = ps["state"] != "printing"
        ps["state"] = "printing" if printing else "standby"
        ps["filename"] = ""
        await push({"print_stats": ps})
    return "ok"


async def handle_rpc(method, params):
    if method == "printer.objects.list":
        return {"objects": list(STATE.keys())}
    if method == "printer.info":
        return {"state": "ready", "state_message": "Printer is ready",
                "hostname": "mock-printer", "software_version": "v0.12-mock"}
    if method == "server.info":
        return {"klippy_connected": True, "klippy_state": "ready",
                "components": ["klipper", "server", "file_manager"],
                "moonraker_version": "mock"}
    if method == "server.files.roots":
        return [{"name": "gcodes", "path": "/tmp/gcodes", "permissions": "rw"}]
    if method == "server.files.list":
        return []
    if method == "printer.objects.subscribe":
        return {"eventtime": time.monotonic(), "status": STATE}
    if method == "server.spoolman.proxy" and BACKEND == "hh":
        # remaining weight lives in spoolman, keyed by gate_spool_id
        return [{"id": sid, "remaining_weight": 950.0 - sid * 95,
                 "filament": {"name": f"Spool {sid}", "material": m}}
                for sid, m in zip(STATE["mmu"]["gate_spool_id"],
                                  STATE["mmu"]["gate_material"]) if sid > 0]
    if method == "printer.gcode.script":
        return await handle_gcode(params.get("script", ""))
    return {}


async def client_handler(ws, path=None):  # path arg for websockets < 11
    print(f"client connected: {ws.remote_address}")
    CLIENTS.add(ws)
    try:
        async for raw in ws:
            try:
                req = json.loads(raw)
            except json.JSONDecodeError:
                continue
            method = req.get("method", "")
            if method != "printer.gcode.script":
                print(f"rpc: {method}")
            # grumpyscreen races LVGL init when responses arrive instantly;
            # emulate real moonraker latency
            await asyncio.sleep(0.2)
            result = await handle_rpc(method, req.get("params", {}))
            if "id" in req:
                await ws.send(json.dumps(
                    {"jsonrpc": "2.0", "id": req["id"], "result": result}))
    finally:
        CLIENTS.discard(ws)
        print("client disconnected")


async def temp_wiggle():
    """Nudge temperatures so the UI visibly live-updates."""
    import math
    t = 0
    while True:
        await asyncio.sleep(1)
        t += 1
        STATE["extruder"]["temperature"] = 24.5 + math.sin(t / 5) * 0.6
        STATE["heater_bed"]["temperature"] = 23.0 + math.cos(t / 7) * 0.4
        await push({"extruder": STATE["extruder"],
                    "heater_bed": STATE["heater_bed"]})


async def main(port):
    asyncio.create_task(temp_wiggle())
    async with websockets.serve(client_handler, "127.0.0.1", port):
        print(f"mock moonraker on ws://127.0.0.1:{port}/websocket")
        await asyncio.Future()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7125)
    ap.add_argument("--lanes", type=int, default=8)
    ap.add_argument("--backend", choices=["afc", "hh"], default="afc",
                    help="afc: native AFC objects; hh: Happy Hare 'mmu' object")
    ap.add_argument("--spoolman", choices=["off", "readonly", "push", "pull"],
                    default="push",
                    help="hh only: 'pull' makes Happy Hare own the gate map, "
                         "so the panel greys out colour/material editing")
    args = ap.parse_args()

    BACKEND = args.backend
    SPOOLMAN_MODE = args.spoolman
    if BACKEND == "hh":
        build_hh(args.lanes)
    else:
        build_lanes(args.lanes)
    asyncio.run(main(args.port))
