#ifndef __AFC_BACKEND_H__
#define __AFC_BACKEND_H__

#include "mmu_backend.h"
#include "websocket_client.h"

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
  KWebSocketClient &ws;
};

#endif // __AFC_BACKEND_H__
