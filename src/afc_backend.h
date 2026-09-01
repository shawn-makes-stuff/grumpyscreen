#ifndef __AFC_BACKEND_H__
#define __AFC_BACKEND_H__

#include "mmu_backend.h"
#include "websocket_client.h"

#include <string>
#include <vector>

class AfcBackend : public MmuBackend {
 public:
  AfcBackend(KWebSocketClient &ws) : ws(ws) {}

  const char *vendor() const override { return "AFC"; }

  bool detect() override;
  bool owns_update(json &j) override;
  void refresh() override;

  void load(int slot) override;
  void change_tool(int slot) override;
  void unload() override;
  void eject(int slot) override;
  void set_colour(int slot, const std::string &hex) override;
  void set_material(int slot, const std::string &material) override;
  void set_backup(int slot, int backup) override;
  void reset_failure() override;
  void dismiss_message() override;

 private:
  // MmuSlot::name is a display name; AFC's gcode needs the raw lane name, so
  // keep them side by side, indexed the same as slots
  const std::string &lane_id(int slot) const {
    static const std::string none;
    return (slot >= 0 && (size_t)slot < lane_ids.size()) ? lane_ids[slot] : none;
  }

  KWebSocketClient &ws;
  std::vector<std::string> lane_ids;
};

#endif // __AFC_BACKEND_H__
