#ifndef __AFC_PANEL_H__
#define __AFC_PANEL_H__

#include "websocket_client.h"
#include "notify_consumer.h"
#include "button_container.h"
#include "lvgl/lvgl.h"

#include <mutex>
#include <string>
#include <vector>

class AfcPanel : public NotifyConsumer {
 public:
  AfcPanel(KWebSocketClient &ws, std::mutex &l);
  ~AfcPanel();

  void foreground();
  void consume(json &j);
  void handle_callback(lv_event_t *e);
  void handle_table_action(lv_event_t *e);

  static void _handle_callback(lv_event_t *event) {
    AfcPanel *panel = (AfcPanel*)event->user_data;
    panel->handle_callback(event);
  };

  static void _handle_table_action(lv_event_t *event) {
    AfcPanel *panel = (AfcPanel*)event->user_data;
    panel->handle_table_action(event);
  };

 private:
  struct Lane {
    std::string name;
    std::string map;
    std::string material;
    std::string color;
    bool prep = false;
    bool load = false;
    bool tool_loaded = false;
    bool loaded_to_hub = false;
  };

  void refresh();
  void populate();

  KWebSocketClient &ws;
  lv_obj_t *cont;
  lv_obj_t *status_label;
  lv_obj_t *lane_table;
  lv_obj_t *controls;
  ButtonContainer unload_btn;
  ButtonContainer reset_btn;
  ButtonContainer back_btn;

  std::vector<Lane> lanes;
  std::string current_load;
  std::string current_state;
  std::string message;
  bool error_state;
  bool bypass;
  bool printing;
};

#endif // __AFC_PANEL_H__
