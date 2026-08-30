#ifndef __AFC_PANEL_H__
#define __AFC_PANEL_H__

#include "websocket_client.h"
#include "notify_consumer.h"
#include "lvgl/lvgl.h"

#include <mutex>
#include <string>
#include <vector>

class AfcPanel : public NotifyConsumer {
 public:
  AfcPanel(KWebSocketClient &ws, std::mutex &l);
  ~AfcPanel();

  // build the UI into the AFC tab. caller must hold lv_lock.
  void create(lv_obj_t *parent);
  // pull current state after the initial subscribe. caller must hold lv_lock.
  void init_state();
  void consume(json &j);

  void handle_card(lv_event_t *e);
  void handle_status_bar(lv_event_t *e);
  void handle_page_prev(lv_event_t *e);
  void handle_page_next(lv_event_t *e);
  void handle_edit_action(lv_event_t *e);
  void handle_dryer_btn(lv_event_t *e);
  void handle_dryer_action(lv_event_t *e);

  static void _handle_card(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_card(e);
  };

  static void _handle_status_bar(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_status_bar(e);
  };

  static void _handle_page_prev(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_page_prev(e);
  };

  static void _handle_page_next(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_page_next(e);
  };

  static void _handle_edit_action(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_edit_action(e);
  };

  static void _handle_dryer_btn(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_dryer_btn(e);
  };

  static void _handle_dryer_action(lv_event_t *e) {
    ((AfcPanel*)e->user_data)->handle_dryer_action(e);
  };

 private:
  struct Lane {
    std::string name;
    std::string map;
    std::string material;
    std::string color;
    std::string runout_lane;
    int spool_id = -1;
    int weight = 0;
    bool prep = false;
    bool load = false;
    bool tool_loaded = false;
    bool loaded_to_hub = false;
  };

  struct Card {
    lv_obj_t *cont;
    lv_obj_t *spool;
    lv_obj_t *checker;
    lv_obj_t *hole;
    lv_obj_t *title;
    lv_obj_t *material;
  };

  struct DryerState {
    bool has_dryer = false;
    bool is_drying = false;
    std::string heater_name;
    std::string temp_sensor_name;
    std::string humidity_sensor_name;
    int current_temp = 0;
    int target_temp = 0;
    int humidity = -1;
    int default_temp = 50;
    int default_time = 240;
  };

  void refresh();
  void populate();
  void rebuild_grid();

  // Full-screen native panels
  void create_edit_screen();
  void open_edit(int idx);
  void close_edit();
  void update_edit_preview();
  void save_edit();

  void create_dryer_screen();
  void open_dryer();
  void close_dryer();
  void update_dryer();
  void update_dryer_btn();
  void set_dryer_target(int temp);

  const char *lane_status(const Lane &lane);
  lv_color_t lane_color(const Lane &lane, bool *valid);
  bool is_backup_lane(const Lane &lane) const;

  KWebSocketClient &ws;
  lv_obj_t *cont;

  // Main Tab Spool Grid
  lv_obj_t *header_row;
  lv_obj_t *status_bar;
  lv_obj_t *status_label;
  lv_obj_t *dryer_btn;
  lv_obj_t *cards_row1;
  lv_obj_t *cards_row2;
  lv_obj_t *nav_row;
  lv_obj_t *nav_prev_btn;
  lv_obj_t *nav_next_btn;
  lv_obj_t *nav_label;
  std::vector<Card> visible_cards;
  size_t current_page;

  // Full-Screen Native Spool Edit Panel (attached to lv_scr_act())
  lv_obj_t *edit_panel_cont;
  lv_obj_t *edit_preview_spool;
  lv_obj_t *edit_preview_checker;
  lv_obj_t *edit_preview_hole;
  lv_obj_t *edit_name_lbl;
  lv_obj_t *edit_tool_lbl;
  lv_obj_t *edit_mat_lbl;
  lv_obj_t *edit_status_lbl;
  lv_obj_t *edit_load_btn;   // toggles between Load and Unload with lane state
  lv_obj_t *edit_eject_btn;
  lv_obj_t *edit_backup_btn; // infinite spool: SET_RUNOUT on the loaded lane
  lv_obj_t *edit_swatches_row1;
  lv_obj_t *edit_swatches_row2;
  std::vector<lv_obj_t*> color_swatch_btns;
  std::vector<std::string> materials;
  std::vector<lv_obj_t*> material_btns;
  lv_obj_t *edit_save_btn;
  lv_obj_t *edit_back_btn;

  // Backup picker popout: choose which lane this one backs up
  lv_obj_t *backup_picker;
  lv_obj_t *backup_picker_list;
  std::vector<lv_obj_t*> backup_pick_btns;
  void open_backup_picker();
  void close_backup_picker();

  // Custom color popout: hue wheel + saturation/brightness sliders
  lv_obj_t *color_picker;
  lv_obj_t *color_wheel;
  lv_obj_t *color_sat_slider;
  lv_obj_t *color_val_slider;
  lv_obj_t *color_pick_preview;
  lv_obj_t *color_pick_ok;
  lv_obj_t *color_pick_cancel;
  lv_obj_t *custom_color_btn;
  void open_color_picker();
  void close_color_picker();
  lv_color_t picker_color();

  // Material picker popout ("more materials")
  lv_obj_t *material_picker;
  lv_obj_t *material_picker_list;
  lv_obj_t *more_mat_btn;
  std::vector<lv_obj_t*> mat_pick_btns;
  void open_material_picker();
  void close_material_picker();

  int edit_lane_idx;
  std::string draft_color;
  std::string draft_material;
  bool draft_dirty = false; // touched drafts survive external lane updates

  // Full-Screen Native MMU Dryer Panel (attached to lv_scr_act()).
  // Controls left, keypad docked right; the temp/time chips are the
  // keypad's edit targets.
  lv_obj_t *dryer_panel_cont;
  lv_obj_t *dryer_kb;        // btnmatrix keypad, always visible
  lv_obj_t *dryer_temp_lbl;
  lv_obj_t *dryer_target_lbl;
  lv_obj_t *dryer_hum_lbl;
  lv_obj_t *dryer_status_lbl;
  lv_obj_t *dryer_quick_title;
  lv_obj_t *dryer_temp_btn;  // custom temp chip, tap to edit
  lv_obj_t *dryer_time_btn;  // custom time chip, tap to edit
  lv_obj_t *dryer_toggle_btn;
  lv_obj_t *dryer_back_btn;
  std::vector<lv_obj_t*> dryer_quick_btns;
  std::vector<int> dryer_quick_temps;

  // custom program the chips edit; Start sends it
  enum class DryerInput { NONE, TEMP, TIME };
  DryerInput dryer_input = DryerInput::NONE;
  std::string dryer_edit_buf;
  int custom_temp = 50;
  int custom_time = 240;

  int dryer_minutes_left = 0;
  lv_timer_t *dryer_tick = NULL;

  static void _dryer_tick_cb(lv_timer_t *t) {
    ((AfcPanel*)t->user_data)->dryer_tick_minute();
  };
  void dryer_tick_minute();
  void commit_dryer_input();

  std::vector<Lane> lanes;
  DryerState dryer;
  std::string current_load;
  std::string current_state;
  std::string message;
  bool error_state;
  bool bypass;
  bool printing;
  bool busy;
  bool spoolman_active = false; // weights only mean something via spoolman
};

#endif // __AFC_PANEL_H__
