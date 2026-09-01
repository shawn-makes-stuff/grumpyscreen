#ifndef __HH_BACKEND_H__
#define __HH_BACKEND_H__

#include "mmu_backend.h"
#include "websocket_client.h"

#include <map>

// Happy Hare driver: reads the single "mmu" printer object, speaks MMU_*.
class HhBackend : public MmuBackend {
 public:
  HhBackend(KWebSocketClient &ws) : ws(ws) {}

  const char *vendor() const override { return "Happy Hare"; }

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

 private:
  void fetch_spoolman_weights();
  void send_groups(const std::vector<int> &groups);
  std::vector<int> current_groups() const;

  KWebSocketClient &ws;

  // HH publishes gate_spool_id but never grams; weights come from spoolman
  std::map<int, int> spool_weights;      // spool id -> grams remaining
  std::vector<int> fetched_spool_ids;    // ids covered by the last fetch

  // endless spool groups sent but not yet echoed back by klipper; bridges
  // the panel's back-to-back clear-then-set command pairs
  std::vector<int> pending_groups;
};

#endif // __HH_BACKEND_H__
