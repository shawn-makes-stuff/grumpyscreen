#ifndef __MMU_BACKEND_H__
#define __MMU_BACKEND_H__

#include "hv/json.hpp"

#include <functional>
#include <string>
#include <vector>

using json = nlohmann::json;

// One filament slot (a lane on AFC, a gate elsewhere) in neutral terms.
struct MmuSlot {
  std::string name;      // backend's display name, unique within the unit
  std::string map;       // tool label(s), e.g. "T0" or "T0,T1" ("" = unmapped)
  std::string material;
  std::string colour;    // "RRGGBB", "" when unset
  int backup = -1;       // slot index that takes over when this one runs out
  int spool_id = -1;
  int weight = 0;        // grams remaining (spoolman), 0 = unknown
  bool prepped = false;  // filament physically present at the slot
  bool ready = false;    // fed into the unit, ready to load
  bool tool_loaded = false;
  // whether colour/material may be edited here. AFC accepts edits on an empty
  // slot, so this is not about filament presence — it is for backends that hand
  // metadata ownership elsewhere, e.g. pulling it from spoolman instead.
  // Backends that never refuse leave it true.
  bool can_configure = true;
};

// The MMU panel renders slots and calls these verbs; it never knows which
// vendor answers. Only concepts every supported backend shares belong here —
// anything vendor-specific stays inside an implementation.
//
// Writing a backend: docs/mmu-backends.md
class MmuBackend {
 public:
  virtual ~MmuBackend() {}

  virtual const char *vendor() const = 0;

  // this backend's objects are present in the current klipper config
  virtual bool detect() = 0;
  // this status delta belongs to this backend
  virtual bool owns_update(json &j) = 0;
  // Rebuild the neutral state below from State; called before the panel
  // redraws, always with the UI lock already held. Do not call changed() from
  // here -- see its note below.
  virtual void refresh() = 0;

  // Verbs; slot arguments index into slots. load and change_tool must both
  // end with that slot loaded to the tool — the panel closes the edit screen
  // on them and waits for loaded_slot to follow. A backend that only moved a
  // selector here would leave the panel showing stale state.
  virtual void load(int slot) = 0;        // load to tool (nothing loaded yet)
  virtual void change_tool(int slot) = 0; // swap the loaded filament for this one
  virtual void unload() = 0;              // unload whatever is in the tool
  virtual void eject(int slot) = 0;       // back out of the unit entirely
  virtual void set_colour(int slot, const std::string &hex) = 0;  // "" clears
  virtual void set_material(int slot, const std::string &material) = 0;
  virtual void set_backup(int slot, int backup) = 0;              // -1 clears
  virtual void reset_failure() = 0;

  // Acknowledge the current `message`. Optional: only backends that hold
  // messages in a queue of their own have anything to do here, and for them a
  // local dismissal is not enough -- an unacknowledged message sits at the head
  // of that queue and hides every later one. Backends whose message is derived
  // from live state have nothing to pop and leave this alone.
  virtual void dismiss_message() {}

  // neutral state, valid after refresh()
  std::vector<MmuSlot> slots;
  int loaded_slot = -1;     // slot currently loaded to the tool
  std::string status_text;  // human text while busy, e.g. "Loading"
  std::string message;      // error/info banner text ("" = none)
  bool error = false;
  bool bypass = false;      // unit bypassed, printing from a single spool
  bool busy = false;        // mid-operation, filament motion is blocked
  bool spoolman = false;    // weights are meaningful

  // Backend-initiated update, for state that arrives outside refresh() -- an
  // async RPC response, typically. Set by the panel; may be null.
  //
  // Call it only from the websocket thread with the UI lock NOT held, i.e.
  // from a response callback. It takes that lock itself, and refresh() and the
  // verbs already run under it, so calling it from any of those deadlocks the
  // UI. Guard against re-entry too: the refresh() it triggers must not fire
  // the same request again.
  std::function<void()> changed;
};

#endif // __MMU_BACKEND_H__
