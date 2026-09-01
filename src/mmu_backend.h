#ifndef __MMU_BACKEND_H__
#define __MMU_BACKEND_H__

#include "hv/json.hpp"

#include <functional>
#include <string>
#include <vector>

using json = nlohmann::json;

// One filament slot (AFC: lane, Happy Hare: gate) in neutral terms.
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
};

// The MMU panel renders slots and calls these verbs; it never knows which
// vendor answers. Only concepts every supported backend shares belong here —
// anything vendor-specific stays inside an implementation.
class MmuBackend {
 public:
  virtual ~MmuBackend() {}

  virtual const char *vendor() const = 0;

  // this backend's objects are present in the current klipper config
  virtual bool detect() = 0;
  // this status delta belongs to this backend
  virtual bool owns_update(json &j) = 0;
  // rebuild the neutral state below from State; called before the panel redraws
  virtual void refresh() = 0;

  // verbs; slot arguments index into slots
  virtual void load(int slot) = 0;        // load to tool (nothing loaded yet)
  virtual void change_tool(int slot) = 0; // swap the loaded filament for this one
  virtual void unload() = 0;              // unload whatever is in the tool
  virtual void eject(int slot) = 0;       // back out of the unit entirely
  virtual void set_colour(int slot, const std::string &hex) = 0;  // "" clears
  virtual void set_material(int slot, const std::string &material) = 0;
  virtual void set_backup(int slot, int backup) = 0;              // -1 clears
  virtual void reset_failure() = 0;

  // neutral state, valid after refresh()
  std::vector<MmuSlot> slots;
  int loaded_slot = -1;     // slot currently loaded to the tool
  std::string status_text;  // human text while busy, e.g. "Loading"
  std::string message;      // error/info banner text ("" = none)
  bool error = false;
  bool bypass = false;      // unit bypassed, printing from a single spool
  bool busy = false;        // mid-operation, filament motion is blocked
  bool spoolman = false;    // weights are meaningful

  // backend-initiated update (async fetches); the panel sets this
  std::function<void()> changed;
};

#endif // __MMU_BACKEND_H__
