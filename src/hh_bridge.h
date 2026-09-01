#ifndef __HH_BRIDGE_H__
#define __HH_BRIDGE_H__

#include "websocket_client.h"
#include "notify_consumer.h"

#include <map>
#include <string>
#include <vector>

class AfcPanel;

// Adapts Happy Hare's moonraker surface (the "mmu" printer object) to the
// AFC-shaped state contract AfcPanel already speaks, in both directions:
//
//   state  in: "mmu" status updates -> synthesized AFC/AFC_lane objects
//   gcode out: the panel's AFC commands -> MMU_* commands (ws rewriter)
//
// The panel stays vendor-agnostic: it renders slots and emits slot commands
// without knowing which backend answers. When the AFC panel PR settles and a
// proper MmuBackend interface is extracted, this class becomes the Happy Hare
// backend; the translation tables below carry over unchanged.
class HappyHareBridge : public NotifyConsumer {
 public:
  HappyHareBridge(KWebSocketClient &ws, std::mutex &lock, AfcPanel &panel);
  ~HappyHareBridge();

  // set by the constructor; the init flow reaches the bridge through this
  // the same way panels reach State
  static HappyHareBridge *instance;

  // true when the printer reports Happy Hare ("mmu" object) and no native
  // AFC. Installs the gcode rewriter on first detection; idempotent.
  bool activate_if_detected();
  // pull current state after the initial subscribe, before the panel's
  // init_state() reads it. no-op unless activated.
  void init_state();
  // ws thread: "mmu" delta arrived -> re-synthesize -> poke the panel
  void consume(json &j);

 private:
  void synthesize();
  void poke_panel();
  std::string rewrite_gcode(const std::string &gcode);
  // HH publishes spool ids but not grams; ask spoolman through moonraker
  void fetch_spoolman_weights();

  static std::string lane_name(int gate);
  static int gate_of(const std::string &lane);

  KWebSocketClient &ws;
  AfcPanel &panel;
  bool active;
  // groups csv sent but not yet echoed back by klipper; keeps back-to-back
  // SET_RUNOUT rewrites (clear old + set new) from computing off stale state
  std::vector<int> pending_groups;
  std::map<int, int> spool_weights;   // spoolman id -> remaining grams
  std::vector<int> fetched_spool_ids; // ids covered by the last fetch
};

#endif // __HH_BRIDGE_H__
