#ifndef __MMU_PANEL_H__
#define __MMU_PANEL_H__

#include "mmu_backend.h"
#include "websocket_client.h"
#include "notify_consumer.h"
#include "loaded_filament.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Vendor-agnostic MMU panel: renders the slots of whichever MmuBackend is
// active and drives it through the MmuBackend verbs only.
class MmuPanel : public NotifyConsumer {
 public:
  MmuPanel(KWebSocketClient &ws, std::mutex &l);
  ~MmuPanel();

  // registration order is priority order when several backends are present
  void add_backend(MmuBackend *b) { backends.push_back(b); }
  // pick the first registered backend present in the klipper config;
  // returns it (NULL when none detected)
  MmuBackend *select_backend();

  // build the UI into the MMU tab. caller must hold lv_lock.
  void create(lv_obj_t *parent);
  // pull current state after the initial subscribe. caller must hold lv_lock.
  void init_state();
  void consume(json &j);

  // notified with a neutral summary whenever the filament loaded to the tool
  // changes (std::nullopt when nothing is loaded). called under lv_lock.
  void set_loaded_filament_cb(std::function<void(const std::optional<LoadedFilament>&)> cb) {
    loaded_filament_cb = std::move(cb);
  }

  void handle_card(lv_event_t *e);
  void handle_status_bar(lv_event_t *e);
  void handle_page_prev(lv_event_t *e);
  void handle_page_next(lv_event_t *e);
  void handle_edit_action(lv_event_t *e);

  static void _handle_card(lv_event_t *e) {
    ((MmuPanel*)e->user_data)->handle_card(e);
  };

  static void _handle_status_bar(lv_event_t *e) {
    ((MmuPanel*)e->user_data)->handle_status_bar(e);
  };

  static void _handle_page_prev(lv_event_t *e) {
    ((MmuPanel*)e->user_data)->handle_page_prev(e);
  };

  static void _handle_page_next(lv_event_t *e) {
    ((MmuPanel*)e->user_data)->handle_page_next(e);
  };

  static void _handle_edit_action(lv_event_t *e) {
    ((MmuPanel*)e->user_data)->handle_edit_action(e);
  };

 private:
  struct Card {
    lv_obj_t *cont;
    lv_obj_t *spool;
    lv_obj_t *checker;
    lv_obj_t *hole;
    lv_obj_t *title;
    lv_obj_t *material;
  };

  void refresh();
  void populate();
  void rebuild_grid();
  void push_loaded_filament();

  // Full-screen native panels
  void create_edit_screen();
  void open_edit(int idx);
  void close_edit();
  void update_edit_preview();
  void save_edit();

  const char *slot_status(const MmuSlot &slot);
  lv_color_t slot_colour(const MmuSlot &slot, bool *valid);
  bool is_backup_slot(int idx) const;

  KWebSocketClient &ws;
  std::vector<MmuBackend*> backends;
  MmuBackend *backend;
  lv_obj_t *cont;

  // Main Tab Spool Grid
  lv_obj_t *header_row;
  lv_obj_t *status_bar;
  lv_obj_t *status_label;
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
  lv_obj_t *edit_load_btn;   // toggles between Load and Unload with slot state
  lv_obj_t *edit_eject_btn;
  lv_obj_t *edit_backup_btn; // infinite spool: backup assignment for the slot
  lv_obj_t *edit_swatches_row1;
  lv_obj_t *edit_swatches_row2;
  std::vector<lv_obj_t*> color_swatch_btns;
  std::vector<std::string> materials;
  std::vector<lv_obj_t*> material_btns;
  lv_obj_t *edit_save_btn;
  lv_obj_t *edit_back_btn;

  // Backup picker popout: choose which slot this one backs up
  lv_obj_t *backup_picker;
  lv_obj_t *backup_picker_list;
  std::vector<lv_obj_t*> backup_pick_btns;
  void open_backup_picker();
  void close_backup_picker();

  // Custom colour popout: hue wheel + saturation/brightness sliders
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
  bool draft_dirty = false; // touched drafts survive external slot updates

  // local copy of the active backend's state, refreshed before each redraw
  std::vector<MmuSlot> lanes;
  int loaded_idx;
  std::string current_state;
  std::string message;
  bool error_state;
  bool bypass;
  bool printing;
  bool busy;
  bool spoolman_active = false; // weights only mean something via spoolman

  std::function<void(const std::optional<LoadedFilament>&)> loaded_filament_cb;
  std::optional<LoadedFilament> last_loaded_filament;
};

#endif // __MMU_PANEL_H__
