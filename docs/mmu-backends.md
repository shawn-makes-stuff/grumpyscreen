# MmuBackend API

`MmuPanel` renders filament slots and drives them through `MmuBackend`
(`src/mmu_backend.h`). It holds no vendor knowledge. Supporting a new
multi-material unit means implementing this interface; the panel is not touched.

Reference implementations: `src/afc_backend.{h,cpp}` (AFC), `src/hh_backend.{h,cpp}`
(Happy Hare).

---

## Implement

All twelve are pure virtual.

### Identity and detection

| Method | Called | Must do |
|---|---|---|
| `const char *vendor() const` | on selection, for the log line | return a display name |
| `bool detect()` | once at startup, before any status data exists | true if this vendor's objects are in `/printer_objs/objects` |
| `bool owns_update(json &j)` | on **every** websocket notification | true if `j` contains this vendor's objects. Keep cheap — returning false skips the redraw |
| `void refresh()` | before every redraw, UI lock held | rebuild **Fill** (below) from `State` |

### Verbs

Fire and forget: send gcode and return. Do not write to `slots` — real state
arrives via the subscription. `slot` is an index into `slots`.

| Method | Contract |
|---|---|
| `void load(int slot)` | ends with that slot loaded to the tool (nothing loaded beforehand) |
| `void change_tool(int slot)` | ends with that slot loaded, swapping out whatever was |
| `void unload()` | filament leaves the tool, **stays in its slot** — slot keeps `prepped`/`ready` |
| `void eject(int slot)` | filament leaves the unit — slot becomes empty |
| `void set_colour(int slot, const std::string &hex)` | `"RRGGBB"`, no `#`. `""` clears |
| `void set_material(int slot, const std::string &material)` | |
| `void set_backup(int slot, int backup)` | `backup` is a slot index, `-1` clears |
| `void reset_failure()` | clear the error state / resume |

---

## Fill

Set by `refresh()`, read by the panel. Clear what you cannot determine rather
than leaving stale values.

### `slots` — `std::vector<MmuSlot>`

| Field | Type | Meaning |
|---|---|---|
| `name` | `string` | display name, unique in the unit (`lane1`, `Gate 0`) |
| `map` | `string` | tool label(s), `T0` or `T0,T1`. `""` = unmapped |
| `material` | `string` | `""` = unset |
| `colour` | `string` | `RRGGBB` no `#`. `""` = unset, draws the checkerboard |
| `backup` | `int` | slot **index** taking over on runout, `-1` = none |
| `spool_id` | `int` | `-1` = none |
| `weight` | `int` | grams remaining, `0` = unknown |
| `prepped` | `bool` | filament physically at the slot |
| `ready` | `bool` | fed into the unit, loadable |
| `tool_loaded` | `bool` | in the toolhead |
| `can_configure` | `bool` | may colour/material be edited here. Default true; set false only when something else owns the metadata |

### Scalars on `MmuBackend`

| Field | Type | Meaning |
|---|---|---|
| `loaded_slot` | `int` | index loaded to the tool, `-1` = none |
| `status_text` | `string` | status bar text while busy (`"Loading"`) |
| `message` | `string` | error banner text, `""` = none |
| `error` | `bool` | banner shown, tapping calls `reset_failure()` |
| `bypass` | `bool` | unit bypassed, single spool |
| `busy` | `bool` | mid-operation, panel blocks filament motion |
| `spoolman` | `bool` | `weight` is meaningful |

---

## Call

### Read printer state

```cpp
State *state = State::get_instance();
json &obj = state->get_data("/printer_state/mmu"_json_pointer);
json &objs = state->get_data("/printer_objs/objects"_json_pointer);
```

`/printer_objs/objects` is the object-name list (use in `detect()`);
`/printer_state/<object>` is live status. Always check `is_null()` / `is_array()`
— fields come and go across vendor versions.

### Send

```cpp
ws.gcode_script("MMU_UNLOAD");                       // fire and forget
ws.send_jsonrpc(method, params, [this](json &d){});  // moonraker RPC + callback
```

`KWebSocketClient &ws` is yours to hold; the panel passes it to your constructor.

### Request a redraw

```cpp
std::function<void()> changed;   // set by the panel, may be null
```

Call `changed()` when state arrives outside a `refresh()` — an async RPC
response, typically. It takes the UI lock, so **never call it synchronously from
inside `refresh()`** (which already runs under that lock) or you deadlock. Guard
against re-entry: the `refresh()` it triggers must not fire the same request
again. See `HhBackend::fetch_spoolman_weights`.

---

## Register

In `GuppyScreen::GuppyScreen()` (`src/guppyscreen.cpp`), as a member so it
outlives the panel:

```cpp
mmu_panel.add_backend(&afc_backend);   // registration order is priority order
mmu_panel.add_backend(&hh_backend);
```

`MmuPanel::select_backend()` runs once at startup and takes the first backend
whose `detect()` passes. If none do, the MMU tab is never created.

---

## Notes

Only shared concepts belong in `MmuBackend`. A feature one vendor has and
another lacks stays inside the implementation and gets no button.

Report what the vendor reports; do not normalise. Whether ejecting clears the
spool colour differs between AFC (a `remember_spool` option), Happy Hare v3
(keeps) and v4 (clears). The panel mirrors metadata and only writes back what
the user changed.

`can_configure` is not about filament presence — both current vendors accept
metadata on an empty slot, and pre-staging one is a real workflow. `HhBackend`
sets it false only when `spoolman_support` is `pull`, where spoolman owns the
gate map and the gcode would be refused.

---

## Test

`tests/mock_moonraker.py` fakes both vendors with no printer attached.

```
./sim.sh                                  # AFC
SIM_BACKEND=hh SIM_LANES=8 ./sim.sh       # Happy Hare
python3 tests/mock_moonraker.py --backend hh --spoolman pull
```

Full simulator setup in DEVELOPMENT.md.
