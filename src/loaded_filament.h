#ifndef __LOADED_FILAMENT_H__
#define __LOADED_FILAMENT_H__

#include <string>

// Neutral summary of the filament currently loaded to the tool, provided by
// whichever MMU backend is active (AFC today, others later). Consumers render
// it without knowing anything about the backend's protocol.
struct LoadedFilament {
  std::string slot;      // display slot/tool, e.g. "T0"
  std::string material;  // e.g. "PLA"

  bool operator==(const LoadedFilament &o) const {
    return slot == o.slot && material == o.material;
  }
  bool operator!=(const LoadedFilament &o) const { return !(*this == o); }
};

#endif // __LOADED_FILAMENT_H__
