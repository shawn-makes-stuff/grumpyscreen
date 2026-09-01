# Writing an MMU backend

`MmuPanel` renders filament slots and drives them through `MmuBackend`. It holds
no vendor knowledge: no gcode, no printer object names, no vendor strings in the
UI. Support for a new multi-material unit means writing a driver, not touching
the panel.

Two exist today, and they are the reference implementations:

| | file | printer object | gcode |
|---|---|---|---|
| AFC | `src/afc_backend.{h,cpp}` | `AFC` + `AFC_lane <name>` | `TOOL_LOAD`, `SET_COLOR`, ... |
| Happy Hare | `src/hh_backend.{h,cpp}` | `mmu` | `MMU_CHANGE_TOOL`, `MMU_GATE_MAP`, ... |

## The shape of a backend

```cpp
class MyBackend : public MmuBackend {
 public:
  MyBackend(KWebSocketClient &ws) : ws(ws) {}
  const char *vendor() const override { return "My MMU"; }
  bool detect() override;
  bool owns_update(json &j) override;
  void refresh() override;
  // ... the eight verbs
 private:
  KWebSocketClient &ws;
};
```

Register it in `GuppyScreen::GuppyScreen()` (`src/guppyscreen.cpp`), as a member
so it outlives the panel:

```cpp
mmu_panel.add_backend(&afc_backend);
mmu_panel.add_backend(&hh_backend);
mmu_panel.add_backend(&my_backend);
```

**Registration order is priority order.** `MmuPanel::select_backend()` runs once
at startup and takes the first backend whose `detect()` passes. AFC is first
because a native install should win over anything that merely fills the gap. If
no backend detects, the MMU tab is never created and the panel allocates nothing.

## Lifecycle

`detect()` — are this vendor's objects in the current klipper config? Read
`/printer_objs/objects` from `State` and look for your object name. Called once,
before the subscription exists, so do not expect any status data yet.

`owns_update(json &j)` — does this status delta concern you? Called on **every**
websocket notification, so keep it cheap: a `contains()` on the object name, or a
short key-prefix scan. Returning false lets the panel skip a redraw entirely,
which is what keeps idle temperature updates from repainting the grid.

`refresh()` — rebuild the neutral state from `State`. Called immediately before
the panel redraws, always with the UI lock held. Populate `slots` and the scalars
below it; clear anything you cannot determine rather than leaving stale values.

The verbs — send gcode. They are fire and forget: do not update `slots` yourself,
because the real state comes back through the subscription and a local guess
would fight it.

## What the neutral state means

Fill in `slots` plus the scalars on `MmuBackend`. The panel never sees anything
else.

`name` is your display name for the slot, unique within the unit — AFC reports
`lane1`, Happy Hare reports `Gate 0`. Do not invent a shared naming scheme; the
panel just prints it.

`map` is the tool label, `T0` or `T0,T1` for a slot serving several tools, empty
when unmapped. `colour` is `RRGGBB` with no leading `#`, empty when unset — an
empty colour is what makes the panel draw the checkerboard rather than a spool.

The three presence flags are a ladder. `prepped` means filament is physically at
the slot, `ready` means it is fed into the unit and can be loaded, `tool_loaded`
means it is in the toolhead. A slot showing none of them is empty.

`backup` is the slot **index** that takes over on runout, or `-1`. Resolve your
vendor's own representation into an index — AFC stores a lane name, Happy Hare
stores endless-spool group numbers, and both convert in the driver. The panel
only understands indices.

`can_configure` says whether colour and material may be edited here. This is not
about filament presence: both existing vendors happily accept metadata on an
empty slot, and pre-staging a slot before loading it is a real workflow. Set it
false only when something else owns the metadata — Happy Hare does this when
`spoolman_support` is `pull`, because spoolman is then the source of truth and
the gcode would be refused.

`busy` blocks filament motion in the UI, `status_text` is what the status bar
shows while busy, `message` is the error banner, `error` makes the banner tappable
to call `reset_failure()`.

## Rules that are easy to get wrong

**Unload is not eject.** `unload()` takes filament out of the toolhead and leaves
it in the slot, so the slot must still report `prepped`/`ready` afterwards.
`eject()` backs it out of the unit and the slot becomes empty. Getting this wrong
makes the whole edit screen grey out after an unload.

**`load()` and `change_tool()` must end with that slot loaded.** The panel closes
the edit screen on them and waits for `loaded_slot` to follow. A backend that only
moved a selector would leave the UI showing stale state. If your vendor has a
select-without-load command, that is an implementation detail, not a verb.

**Do not normalise vendor behaviour.** Whether ejecting clears the spool colour
differs between AFC (a `remember_spool` config option), Happy Hare v3 (keeps it)
and Happy Hare v4 (clears it). Report what the vendor reports. The panel mirrors
metadata, it does not own it, and it only writes back values the user actually
changed.

**Only shared concepts belong in `MmuBackend`.** If one vendor has a feature
another lacks, it does not go in the interface and it does not get a button.
Keep it inside the implementation.

## Threading

`refresh()` and the verbs are called with the UI lock held. If you need an async
fetch — Happy Hare pulls spool weights from moonraker because it publishes spool
ids but never grams — send the request from `refresh()` and call `changed()` from
the **response callback**, never synchronously from inside `refresh()`. `changed()`
takes the UI lock itself, so calling it inline would deadlock.

Guard async fetches against re-entry. `HhBackend` records the spool ids it has
already fetched, so the `refresh()` triggered by its own `changed()` does not
fire another request.

## Testing without hardware

`tests/mock_moonraker.py` fakes both vendors. Add a mode for yours and you can
exercise the panel end to end with no printer:

```
./sim.sh                                  # AFC
SIM_BACKEND=hh SIM_LANES=8 ./sim.sh       # Happy Hare
python3 tests/mock_moonraker.py --backend hh --spoolman pull
```

The mock handles the gcode the panel sends and pushes `notify_status_update`
back, so interactions round trip. See DEVELOPMENT.md for the full simulator
setup.
