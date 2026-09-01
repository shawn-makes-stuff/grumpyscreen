#include "mmu_panel.h"
#include "config.h"
#include "state.h"
#include "utils.h"
#include "logger.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <sstream>

LV_IMG_DECLARE(back);

// 10 classic filament colors + Clear/None; with the custom button this
// fills an even 2x6 grid
static const char *COLOR_PRESETS[] = {
  "", "212121", "FFFFFF", "9E9E9E", "F44336", "FF9800",
  "FFEB3B", "4CAF50", "2196F3", "9C27B0", "795548"
};

// Material presets fallback when /mmu/materials is not configured
static const char *MATERIAL_PRESETS[] = {
  "PLA", "PETG", "ABS", "TPU"
};

// The material popout list; the inline row shows the common four
static const char *MATERIAL_CATALOG[] = {
  "PLA", "PLA+", "PLA-CF", "PETG", "PETG-CF", "ABS", "ABS-CF", "ASA",
  "TPU", "PC", "PA", "PA-CF", "PVA", "HIPS"
};

// The edit screen material row fits this many buttons plus the "more" button
static const size_t MAX_MATERIALS = 4;

// "lane12" reads awkwardly as a title; show "Lane 12" (custom names pass through)
static std::string pretty_lane_name(const std::string &name) {
  if (name.rfind("lane", 0) == 0 && name.size() > 4 &&
      std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
    return fmt::format("Lane {}", name.substr(4));
  }
  return name;
}

static std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok.erase(0, tok.find_first_not_of(" \t"));
    tok.erase(tok.find_last_not_of(" \t") + 1);
    if (!tok.empty()) out.push_back(tok);
  }
  return out;
}

static const int HEADER_HEIGHT = 34;
static const size_t CARDS_PER_PAGE = 8;
static const size_t CARDS_PER_ROW = 4;

// the design baseline is the 480x272 small screen: every structural size
// below is that design times the current display scale, so any resolution
// renders the same layout, just larger. at 480x272 all helpers are identity
static int scale_w(int px) { return px * lv_disp_get_physical_hor_res(NULL) / 480; }
static int scale_h(int px) { return px * lv_disp_get_physical_ver_res(NULL) / 272; }
// squares and circles follow the tighter axis so they stay round
static int scale_r(int px) { return std::min(scale_w(px), scale_h(px)); }

// one gap everywhere on the lane grid: screen edges, header, rows, cards
static int grid_gap() { return scale_r(6); }
// popout boxes span the screen minus an even margin on every side
static int popout_w() { return lv_disp_get_physical_hor_res(NULL) - 2 * scale_r(8); }
static int popout_max_h() { return lv_disp_get_physical_ver_res(NULL) - 2 * scale_r(8); }
// usable row width inside a popout: box padding, 1px borders, and a little
// headroom so integer rounding can never wrap a full row of tiles
static int popout_row_w() { return popout_w() - 2 * scale_r(10) - 4; }

// text scales with the layout: snap to the smallest enabled montserrat font
// that fits the scaled size (largest available otherwise). at 480x272 every
// lookup returns the requested size unchanged
static const lv_font_t *scale_font(int px) {
  struct F { int size; const lv_font_t *font; };
  static const F fonts[] = {
    {12, &lv_font_montserrat_12}, {14, &lv_font_montserrat_14},
    {16, &lv_font_montserrat_16}, {18, &lv_font_montserrat_18},
    {20, &lv_font_montserrat_20}, {22, &lv_font_montserrat_22},
#if LV_FONT_MONTSERRAT_24
    {24, &lv_font_montserrat_24},
#endif
#if LV_FONT_MONTSERRAT_26
    {26, &lv_font_montserrat_26},
#endif
#if LV_FONT_MONTSERRAT_28
    {28, &lv_font_montserrat_28},
#endif
  };
  int target = scale_r(px);
  for (const F &f : fonts) {
    if (f.size >= target) return f.font;
  }
  return fonts[sizeof(fonts) / sizeof(fonts[0]) - 1].font;
}

static lv_color_t theme_primary() {
  // config never changes at runtime; parse once
  static const lv_color_t c = lv_color_hex(std::stoul(
      Config::get_instance()->get<std::string>("/theme/primary_colour", "0x2196F3"), nullptr, 16));
  return c;
}

static lv_obj_t *create_flat_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  lv_obj_set_style_text_font(btn, scale_font(14), 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_transform_width(btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(btn, -2, LV_STATE_PRESSED);
  if (cb != NULL) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
  }
  return btn;
}

static void set_btn_label(lv_obj_t *btn, const char *text) {
  if (btn != NULL && lv_obj_get_child_cnt(btn) > 0) {
    lv_label_set_text(lv_obj_get_child(btn, 0), text);
  }
}

static void set_action_btn(lv_obj_t *btn, bool enabled, lv_color_t enabled_color) {
  if (enabled) {
    lv_obj_clear_state(btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn, enabled_color, 0);
  } else {
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  }
}

// Generates a 4x4 PNG-style transparency grid (alternating dark/light grey tiles)
static lv_obj_t *create_checker_pattern(lv_obj_t *parent, int diameter) {
  lv_obj_t *chk = lv_obj_create(parent);
  lv_obj_set_size(chk, diameter, diameter);
  lv_obj_set_style_pad_all(chk, 0, 0);
  lv_obj_set_style_border_width(chk, 0, 0);
  lv_obj_set_style_radius(chk, 0, 0);
  lv_obj_set_style_bg_opa(chk, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(chk, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(chk, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(chk);

  int grid_size = 4;
  int tile_size = (diameter + grid_size - 1) / grid_size;

  for (int r = 0; r < grid_size; r++) {
    for (int c = 0; c < grid_size; c++) {
      lv_obj_t *tile = lv_obj_create(chk);
      lv_obj_set_pos(tile, c * tile_size, r * tile_size);
      lv_obj_set_size(tile, tile_size, tile_size);
      lv_obj_set_style_radius(tile, 0, 0);
      lv_obj_set_style_border_width(tile, 0, 0);
      lv_obj_set_style_pad_all(tile, 0, 0);
      lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);

      bool alt = (r + c) % 2 == 0;
      lv_obj_set_style_bg_color(tile, alt ? lv_palette_darken(LV_PALETTE_GREY, 1) : lv_palette_darken(LV_PALETTE_GREY, 4), 0);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    }
  }
  return chk;
}

static void style_spool_icon(lv_obj_t *spool, lv_obj_t *hole, lv_obj_t **checker, int diameter) {
  lv_obj_set_size(spool, diameter, diameter);
  lv_obj_set_style_pad_all(spool, 0, 0);
  lv_obj_set_style_radius(spool, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(spool, true, 0);
  lv_obj_set_style_border_width(spool, 2, 0);
  lv_obj_clear_flag(spool, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(spool, LV_OBJ_FLAG_CLICKABLE);

  if (checker != NULL) {
    *checker = create_checker_pattern(spool, diameter);
  }

  int hole_size = std::max(10, diameter / 3);
  lv_obj_set_size(hole, hole_size, hole_size);
  lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
  lv_obj_set_style_border_width(hole, 0, 0);
  lv_obj_center(hole);
  lv_obj_move_foreground(hole);
  lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(hole, LV_OBJ_FLAG_CLICKABLE);
}

static void paint_spool_icon(lv_obj_t *spool, lv_obj_t *hole, lv_obj_t *checker, lv_color_t color,
                             bool color_valid, bool has_filament, bool tool_loaded, lv_color_t primary) {
  if (tool_loaded) {
    if (checker != NULL) lv_obj_add_flag(checker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(spool, color, 0);
    lv_obj_set_style_bg_opa(spool, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(spool, primary, 0);
    lv_obj_set_style_border_width(spool, 3, 0);
    if (hole != NULL) {
      lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
      lv_obj_set_style_border_color(hole, primary, 0);
      lv_obj_set_style_border_width(hole, 2, 0);
    }
  } else if (has_filament) {
    // Ready (assumed normal state). Dark filament blends into the card
    // background, so give it a grey rim instead of a darkened one
    if (checker != NULL) lv_obj_add_flag(checker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(spool, color, 0);
    lv_obj_set_style_bg_opa(spool, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(spool, lv_color_brightness(color) < 60
                                  ? lv_palette_main(LV_PALETTE_GREY)
                                  : lv_color_darken(color, LV_OPA_30), 0);
    lv_obj_set_style_border_width(spool, 2, 0);
    if (hole != NULL) {
      lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
      lv_obj_set_style_border_color(hole, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
      lv_obj_set_style_border_width(hole, lv_color_brightness(color) < 60 ? 1 : 0, 0);
    }
  } else if (color_valid) {
    // Empty but a color is configured: show it translucent so fill state stays readable
    if (checker != NULL) lv_obj_add_flag(checker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(spool, color, 0);
    lv_obj_set_style_bg_opa(spool, LV_OPA_50, 0);
    lv_obj_set_style_border_color(spool, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_border_width(spool, 1, 0);
    if (hole != NULL) {
      lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
      lv_obj_set_style_border_width(hole, 0, 0);
    }
  } else {
    // Empty spool, no color -> Show Alpha / PNG checkerboard grid pattern
    if (checker != NULL) lv_obj_clear_flag(checker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(spool, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(spool, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_border_width(spool, 1, 0);
    if (hole != NULL) {
      lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
      lv_obj_set_style_border_color(hole, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
      lv_obj_set_style_border_width(hole, 1, 0);
    }
  }
}

MmuPanel::MmuPanel(KWebSocketClient &c, std::mutex &l)
  : NotifyConsumer(l)
  , ws(c)
  , backend(NULL)
  , cont(NULL)
  , header_row(NULL)
  , status_bar(NULL)
  , status_label(NULL)
  , cards_row1(NULL)
  , cards_row2(NULL)
  , nav_row(NULL)
  , nav_prev_btn(NULL)
  , nav_next_btn(NULL)
  , nav_label(NULL)
  , current_page(0)
  , edit_panel_cont(NULL)
  , edit_preview_spool(NULL)
  , edit_preview_checker(NULL)
  , edit_preview_hole(NULL)
  , edit_name_lbl(NULL)
  , edit_tool_lbl(NULL)
  , edit_mat_lbl(NULL)
  , edit_status_lbl(NULL)
  , edit_load_btn(NULL)
  , edit_eject_btn(NULL)
  , edit_backup_btn(NULL)
  , edit_swatches_row1(NULL)
  , edit_swatches_row2(NULL)
  , edit_save_btn(NULL)
  , edit_back_btn(NULL)
  , backup_picker(NULL)
  , backup_picker_list(NULL)
  , color_picker(NULL)
  , color_wheel(NULL)
  , color_sat_slider(NULL)
  , color_val_slider(NULL)
  , color_pick_preview(NULL)
  , color_pick_ok(NULL)
  , color_pick_cancel(NULL)
  , custom_color_btn(NULL)
  , material_picker(NULL)
  , material_picker_list(NULL)
  , more_mat_btn(NULL)
  , edit_lane_idx(-1)
  , loaded_idx(-1)
  , error_state(false)
  , bypass(false)
  , printing(false)
  , busy(false)
{
  // screens are built lazily in create() so printers without AFC
  // never allocate any of this panel's LVGL objects
  ws.register_notify_update(this);
}

MmuPanel::~MmuPanel() {
  if (backup_picker != NULL) {
    lv_obj_del(backup_picker);
    backup_picker = NULL;
  }
  if (color_picker != NULL) {
    lv_obj_del(color_picker);
    color_picker = NULL;
  }
  if (material_picker != NULL) {
    lv_obj_del(material_picker);
    material_picker = NULL;
  }
  if (edit_panel_cont != NULL) {
    lv_obj_del(edit_panel_cont);
    edit_panel_cont = NULL;
  }
  // cont is owned by the tabview; MainPanel deletes it with the tab
}

// =========================================================================
// MAIN TAB VIEW: paginated lane grid with status header
// =========================================================================
void MmuPanel::create(lv_obj_t *parent) {
  if (cont != NULL) {
    return;
  }

  create_edit_screen();

  cont = lv_obj_create(parent);
  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(cont, grid_gap(), 0);
  lv_obj_set_style_pad_row(cont, grid_gap(), 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  // Top Header Row (status container)
  header_row = lv_obj_create(cont);
  lv_obj_set_size(header_row, LV_PCT(100), scale_h(HEADER_HEIGHT));
  lv_obj_set_style_pad_all(header_row, 0, 0);
  lv_obj_set_style_pad_column(header_row, scale_r(4), 0);
  lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);

  // Status Bar inside header row (Flex grow fills available space)
  status_bar = lv_obj_create(header_row);
  lv_obj_set_height(status_bar, LV_PCT(100));
  lv_obj_set_flex_grow(status_bar, 1);
  lv_obj_set_style_radius(status_bar, scale_r(6), 0);
  lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_GREY, 3), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(status_bar, 1, 0);
  lv_obj_set_style_border_color(status_bar, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_transform_width(status_bar, -2, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(status_bar, -2, LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(status_bar, scale_r(12), 0);
  lv_obj_set_style_pad_ver(status_bar, 0, 0);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(status_bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(status_bar, &MmuPanel::_handle_status_bar, LV_EVENT_CLICKED, this);

  status_label = lv_label_create(status_bar);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
  lv_label_set_text(status_label, "MMU Standby");
  lv_obj_set_style_text_font(status_label, scale_font(12), 0);
  lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_width(status_label, LV_PCT(100));
  lv_obj_clear_flag(status_label, LV_OBJ_FLAG_CLICKABLE);

  // Row 1: Spools 1 - 4. The card rows flex-grow to split whatever height
  // the header and nav rows leave over
  cards_row1 = lv_obj_create(cont);
  lv_obj_set_width(cards_row1, LV_PCT(100));
  lv_obj_set_flex_grow(cards_row1, 1);
  lv_obj_set_style_pad_all(cards_row1, 0, 0);
  lv_obj_set_style_pad_column(cards_row1, grid_gap(), 0);
  lv_obj_clear_flag(cards_row1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cards_row1, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cards_row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Row 2: Spools 5 - 8
  cards_row2 = lv_obj_create(cont);
  lv_obj_set_width(cards_row2, LV_PCT(100));
  lv_obj_set_flex_grow(cards_row2, 1);
  lv_obj_set_style_pad_all(cards_row2, 0, 0);
  lv_obj_set_style_pad_column(cards_row2, grid_gap(), 0);
  lv_obj_clear_flag(cards_row2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cards_row2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cards_row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Row 3: Page navigation (if > 8 spools)
  nav_row = lv_obj_create(cont);
  lv_obj_set_size(nav_row, LV_PCT(100), scale_h(26));
  lv_obj_set_style_pad_all(nav_row, 0, 0);
  lv_obj_clear_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(nav_row, LV_OBJ_FLAG_HIDDEN);

  nav_prev_btn = create_flat_btn(nav_row, "< Prev", &MmuPanel::_handle_page_prev, this);
  lv_obj_set_size(nav_prev_btn, scale_w(70), scale_h(24));
  lv_obj_align(nav_prev_btn, LV_ALIGN_LEFT_MID, scale_w(4), 0);
  lv_obj_set_style_radius(nav_prev_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(nav_prev_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);

  nav_label = lv_label_create(nav_row);
  lv_label_set_text(nav_label, "Page 1 / 1");
  lv_obj_set_style_text_font(nav_label, scale_font(12), 0);
  lv_obj_center(nav_label);

  nav_next_btn = create_flat_btn(nav_row, "Next >", &MmuPanel::_handle_page_next, this);
  lv_obj_set_size(nav_next_btn, scale_w(70), scale_h(24));
  lv_obj_align(nav_next_btn, LV_ALIGN_RIGHT_MID, -scale_w(4), 0);
  lv_obj_set_style_radius(nav_next_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(nav_next_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
}

// =========================================================================
// FULL-SCREEN SPOOL EDIT PANEL (480x272 on lv_scr_act())
// =========================================================================
void MmuPanel::create_edit_screen() {
  if (edit_panel_cont != NULL) return;

  lv_color_t primary = theme_primary();

  edit_panel_cont = lv_obj_create(lv_scr_act());
  lv_obj_set_size(edit_panel_cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(edit_panel_cont, scale_r(6), 0);
  lv_obj_set_style_pad_column(edit_panel_cont, scale_r(8), 0);
  lv_obj_clear_flag(edit_panel_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(edit_panel_cont, LV_FLEX_FLOW_ROW);

  lv_obj_add_flag(edit_panel_cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(edit_panel_cont);

  // Left Column: preview, info and lane actions
  lv_obj_t *left_col = lv_obj_create(edit_panel_cont);
  lv_obj_set_size(left_col, scale_w(185), LV_PCT(100));
  lv_obj_set_style_pad_all(left_col, scale_r(6), 0);
  lv_obj_set_style_radius(left_col, scale_r(8), 0);
  lv_obj_set_style_bg_color(left_col, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_border_width(left_col, 1, 0);
  lv_obj_set_style_border_color(left_col, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Left Top Info Box
  lv_obj_t *preview_box = lv_obj_create(left_col);
  lv_obj_set_size(preview_box, LV_PCT(100), scale_h(160));
  lv_obj_set_style_pad_all(preview_box, scale_r(2), 0);
  lv_obj_set_style_bg_opa(preview_box, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(preview_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(preview_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(preview_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  edit_name_lbl = lv_label_create(preview_box);
  lv_label_set_text(edit_name_lbl, "Lane 1");
  lv_obj_set_style_text_font(edit_name_lbl, scale_font(14), 0);

  edit_preview_spool = lv_obj_create(preview_box);
  edit_preview_hole = lv_obj_create(edit_preview_spool);
  style_spool_icon(edit_preview_spool, edit_preview_hole, &edit_preview_checker, scale_r(48));

  edit_mat_lbl = lv_label_create(preview_box);
  lv_label_set_text(edit_mat_lbl, "-");
  lv_obj_set_style_text_font(edit_mat_lbl, scale_font(12), 0);

  edit_tool_lbl = lv_label_create(preview_box);
  lv_label_set_text(edit_tool_lbl, "Tool: T0");
  lv_obj_set_style_text_font(edit_tool_lbl, scale_font(12), 0);
  lv_obj_set_style_text_color(edit_tool_lbl, primary, 0);

  edit_status_lbl = lv_label_create(preview_box);
  lv_label_set_text(edit_status_lbl, "Status: Ready");
  lv_obj_set_style_text_font(edit_status_lbl, scale_font(12), 0);
  lv_obj_set_style_text_color(edit_status_lbl, lv_palette_main(LV_PALETTE_GREY), 0);

  // Left Bottom Actions Box: Load/Unload toggle + Eject
  lv_obj_t *left_actions = lv_obj_create(left_col);
  lv_obj_set_size(left_actions, LV_PCT(100), scale_h(76));
  lv_obj_set_style_pad_all(left_actions, 0, 0);
  lv_obj_set_style_pad_row(left_actions, scale_r(6), 0);
  lv_obj_set_style_bg_opa(left_actions, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(left_actions, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(left_actions, LV_FLEX_FLOW_COLUMN);

  edit_load_btn = create_flat_btn(left_actions, "Load", &MmuPanel::_handle_edit_action, this);
  lv_obj_set_size(edit_load_btn, LV_PCT(100), scale_h(38));
  lv_obj_set_style_radius(edit_load_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(edit_load_btn, primary, 0);

  edit_eject_btn = create_flat_btn(left_actions, "Eject Spool", &MmuPanel::_handle_edit_action, this);
  lv_obj_set_size(edit_eject_btn, LV_PCT(100), scale_h(32));
  lv_obj_set_style_radius(edit_eject_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(edit_eject_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);

  // Right Column: color presets, material, backup, save/back
  lv_obj_t *right_col = lv_obj_create(edit_panel_cont);
  lv_obj_set_height(right_col, LV_PCT(100));
  lv_obj_set_flex_grow(right_col, 1);
  lv_obj_set_style_pad_all(right_col, scale_r(6), 0);
  lv_obj_set_style_radius(right_col, scale_r(8), 0);
  lv_obj_set_style_bg_color(right_col, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_border_width(right_col, 1, 0);
  lv_obj_set_style_border_color(right_col, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_clear_flag(right_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

  // 1. Color Presets (2x6 grid including the custom button)
  lv_obj_t *color_sec = lv_obj_create(right_col);
  lv_obj_set_size(color_sec, LV_PCT(100), scale_h(76));
  lv_obj_set_style_pad_all(color_sec, 0, 0);
  lv_obj_set_style_bg_opa(color_sec, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(color_sec, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *col_title = lv_label_create(color_sec);
  lv_label_set_text(col_title, "COLOR PRESETS:");
  lv_obj_set_style_text_font(col_title, scale_font(12), 0);
  lv_obj_set_style_text_color(col_title, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(col_title, LV_ALIGN_TOP_LEFT, 0, 0);

  edit_swatches_row1 = lv_obj_create(color_sec);
  lv_obj_set_size(edit_swatches_row1, LV_PCT(100), scale_h(26));
  lv_obj_set_style_pad_all(edit_swatches_row1, 0, 0);
  lv_obj_set_style_pad_column(edit_swatches_row1, scale_r(6), 0);
  lv_obj_set_style_bg_opa(edit_swatches_row1, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(edit_swatches_row1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(edit_swatches_row1, LV_FLEX_FLOW_ROW);
  lv_obj_align(edit_swatches_row1, LV_ALIGN_TOP_LEFT, 0, scale_h(18));

  edit_swatches_row2 = lv_obj_create(color_sec);
  lv_obj_set_size(edit_swatches_row2, LV_PCT(100), scale_h(26));
  lv_obj_set_style_pad_all(edit_swatches_row2, 0, 0);
  lv_obj_set_style_pad_column(edit_swatches_row2, scale_r(6), 0);
  lv_obj_set_style_bg_opa(edit_swatches_row2, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(edit_swatches_row2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(edit_swatches_row2, LV_FLEX_FLOW_ROW);
  lv_obj_align(edit_swatches_row2, LV_ALIGN_TOP_LEFT, 0, scale_h(48));

  color_swatch_btns.clear();
  for (size_t c_idx = 0; c_idx < sizeof(COLOR_PRESETS)/sizeof(COLOR_PRESETS[0]); c_idx++) {
    const char *hex_str = COLOR_PRESETS[c_idx];
    lv_obj_t *parent_row = (c_idx < 6) ? edit_swatches_row1 : edit_swatches_row2;

    lv_obj_t *swatch = lv_btn_create(parent_row);
    lv_obj_set_height(swatch, LV_PCT(100));
    lv_obj_set_flex_grow(swatch, 1);
    lv_obj_set_style_radius(swatch, scale_r(4), 0);
    lv_obj_set_style_shadow_width(swatch, 0, 0);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_transform_width(swatch, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(swatch, -2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_user_data(swatch, (void*)hex_str);

    if (c_idx == 0) {
      lv_obj_set_style_bg_color(swatch, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
      lv_obj_t *icon = lv_label_create(swatch);
      lv_label_set_text(icon, LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_font(icon, scale_font(12), 0);
      lv_obj_center(icon);
      lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_GREY), 0);
    } else {
      lv_color_t c = lv_color_hex(std::stoul(hex_str, nullptr, 16));
      lv_obj_set_style_bg_color(swatch, c, 0);
      lv_obj_set_style_bg_color(swatch, c, LV_STATE_PRESSED);
    }
    lv_obj_add_event_cb(swatch, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);
    color_swatch_btns.push_back(swatch);
  }

  // custom color: opens the colorwheel popout
  custom_color_btn = lv_btn_create(edit_swatches_row2);
  lv_obj_set_height(custom_color_btn, LV_PCT(100));
  lv_obj_set_flex_grow(custom_color_btn, 1);
  lv_obj_set_style_radius(custom_color_btn, scale_r(4), 0);
  lv_obj_set_style_shadow_width(custom_color_btn, 0, 0);
  lv_obj_set_style_pad_all(custom_color_btn, 0, 0);
  lv_obj_set_style_border_width(custom_color_btn, 1, 0);
  lv_obj_set_style_border_color(custom_color_btn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_set_style_bg_color(custom_color_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_transform_width(custom_color_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(custom_color_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(custom_color_btn, LV_OPA_30, LV_STATE_DISABLED);
  lv_obj_t *cc_icon = lv_label_create(custom_color_btn);
  lv_label_set_text(cc_icon, LV_SYMBOL_EDIT);
  lv_obj_set_style_text_font(cc_icon, scale_font(12), 0);
  lv_obj_center(cc_icon);
  lv_obj_add_event_cb(custom_color_btn, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);

  // 2. Materials: inline commons plus the catalog popout
  lv_obj_t *mat_sec = lv_obj_create(right_col);
  lv_obj_set_size(mat_sec, LV_PCT(100), scale_h(54));
  lv_obj_set_style_pad_all(mat_sec, 0, 0);
  lv_obj_set_style_bg_opa(mat_sec, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(mat_sec, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *mat_title = lv_label_create(mat_sec);
  lv_label_set_text(mat_title, "MATERIAL:");
  lv_obj_set_style_text_font(mat_title, scale_font(12), 0);
  lv_obj_set_style_text_color(mat_title, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(mat_title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *mat_row = lv_obj_create(mat_sec);
  lv_obj_set_size(mat_row, LV_PCT(100), scale_h(34));
  lv_obj_set_style_pad_all(mat_row, 0, 0);
  lv_obj_set_style_pad_column(mat_row, scale_r(4), 0);
  lv_obj_set_style_bg_opa(mat_row, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(mat_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(mat_row, LV_FLEX_FLOW_ROW);
  lv_obj_align(mat_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  materials = split_csv(Config::get_instance()->get<std::string>("/mmu/materials", ""));
  if (materials.empty()) {
    materials.assign(std::begin(MATERIAL_PRESETS), std::end(MATERIAL_PRESETS));
  }
  if (materials.size() > MAX_MATERIALS) {
    LOG_INFO("/mmu/materials has {} values; truncating to {}", materials.size(), MAX_MATERIALS);
    materials.resize(MAX_MATERIALS);
  }

  material_btns.clear();
  for (const auto &mat_name : materials) {
    lv_obj_t *b = create_flat_btn(mat_row, mat_name.c_str(), &MmuPanel::_handle_edit_action, this);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, scale_r(4), 0);
    lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_user_data(b, (void*)mat_name.c_str());
    material_btns.push_back(b);
  }

  // more materials: opens the catalog popout
  more_mat_btn = lv_btn_create(mat_row);
  lv_obj_set_size(more_mat_btn, scale_w(38), LV_PCT(100));
  lv_obj_set_style_radius(more_mat_btn, scale_r(4), 0);
  lv_obj_set_style_shadow_width(more_mat_btn, 0, 0);
  lv_obj_set_style_pad_all(more_mat_btn, 0, 0);
  lv_obj_set_style_bg_color(more_mat_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_transform_width(more_mat_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(more_mat_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(more_mat_btn, LV_OPA_30, LV_STATE_DISABLED);
  lv_obj_t *mm_icon = lv_label_create(more_mat_btn);
  lv_label_set_text(mm_icon, LV_SYMBOL_LIST);
  lv_obj_set_style_text_font(mm_icon, scale_font(12), 0);
  lv_obj_center(mm_icon);
  lv_obj_add_event_cb(more_mat_btn, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);

  // 3. Infinite spool: the button is self-descriptive, no section title
  edit_backup_btn = create_flat_btn(right_col, "Use as Backup", &MmuPanel::_handle_edit_action, this);
  lv_obj_set_size(edit_backup_btn, LV_PCT(100), scale_h(40));
  lv_obj_set_style_radius(edit_backup_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(edit_backup_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);

  // 4. Save / Back row
  lv_obj_t *save_row = lv_obj_create(right_col);
  lv_obj_set_size(save_row, LV_PCT(100), scale_h(46));
  lv_obj_set_style_pad_all(save_row, 0, 0);
  lv_obj_set_style_pad_column(save_row, scale_r(8), 0);
  lv_obj_set_style_bg_opa(save_row, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(save_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(save_row, LV_FLEX_FLOW_ROW);

  edit_save_btn = create_flat_btn(save_row, "Save", &MmuPanel::_handle_edit_action, this);
  lv_obj_set_height(edit_save_btn, LV_PCT(100));
  lv_obj_set_flex_grow(edit_save_btn, 1);
  lv_obj_set_style_radius(edit_save_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(edit_save_btn, primary, 0);

  edit_back_btn = lv_btn_create(save_row);
  lv_obj_set_height(edit_back_btn, LV_PCT(100));
  lv_obj_set_width(edit_back_btn, scale_w(76));
  lv_obj_set_style_radius(edit_back_btn, scale_r(4), 0);
  lv_obj_set_style_bg_color(edit_back_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_bg_color(edit_back_btn, lv_palette_darken(LV_PALETTE_GREY, 2), LV_STATE_PRESSED);
  lv_obj_set_style_transform_width(edit_back_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(edit_back_btn, -2, LV_STATE_PRESSED);
  lv_obj_set_style_pad_all(edit_back_btn, scale_r(4), 0);
  lv_obj_t *back_icon = lv_img_create(edit_back_btn);
  lv_img_set_src(back_icon, &back);
  lv_img_set_zoom(back_icon, 180 * scale_r(100) / 100);
  lv_obj_center(back_icon);
  lv_obj_add_event_cb(edit_back_btn, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);
}

// Registration order is priority order: a native AFC install wins over a
// backend that only fills the gap.
MmuBackend *MmuPanel::select_backend() {
  backend = NULL;
  for (auto *b : backends) {
    if (b->detect()) {
      LOG_INFO("MMU backend: {}", b->vendor());
      backend = b;
      backend->changed = [this]() {
        std::lock_guard<std::mutex> lock(lv_lock);
        if (cont == NULL) return;
        refresh();
        push_loaded_filament();
        populate();
      };
      break;
    }
  }
  return backend;
}

void MmuPanel::init_state() {
  if (cont == NULL) return;
  refresh();
  push_loaded_filament();
  populate();
}

// tell whoever registered (the print status screen) what is loaded to the
// tool, in backend-neutral terms. only fires when the summary changes.
void MmuPanel::push_loaded_filament() {
  if (!loaded_filament_cb) return;

  std::optional<LoadedFilament> summary;
  if (loaded_idx >= 0 && (size_t)loaded_idx < lanes.size()) {
    const MmuSlot &lane = lanes[loaded_idx];
    summary = LoadedFilament{lane.map.empty() ? lane.name : lane.map, lane.material};
  }

  if (summary != last_loaded_filament) {
    last_loaded_filament = summary;
    loaded_filament_cb(summary);
  }
}

void MmuPanel::refresh() {
  State *state = State::get_instance();
  json &pstat = state->get_data("/printer_state/print_stats/state"_json_pointer);
  printing = !pstat.is_null() && pstat.template get<std::string>() == "printing";

  lanes.clear();
  loaded_idx = -1;
  current_state = "";
  message = "";
  error_state = false;
  bypass = false;
  busy = false;
  spoolman_active = false;

  if (backend == NULL) return;

  backend->refresh();
  lanes = backend->slots;
  loaded_idx = backend->loaded_slot;
  current_state = backend->status_text;
  message = backend->message;
  error_state = backend->error;
  bypass = backend->bypass;
  busy = backend->busy;
  spoolman_active = backend->spoolman;

  // stop suppressing once the backend drops the message, so the same text
  // arriving again is shown rather than silently swallowed
  if (message.empty()) dismissed_message.clear();
}

const char *MmuPanel::slot_status(const MmuSlot &slot) {
  if (slot.tool_loaded) return "Loaded";
  if (slot.ready) return "Ready";
  if (slot.prepped) return "Present"; // detected, not yet fed into the unit
  return "Empty";
}

// a slot is a backup when another slot names it as its backup (infinite spool)
bool MmuPanel::is_backup_slot(int idx) const {
  for (size_t i = 0; i < lanes.size(); i++) {
    if ((int)i != idx && lanes[i].backup == idx) return true;
  }
  return false;
}

lv_color_t MmuPanel::slot_colour(const MmuSlot &slot, bool *valid) {
  std::string colour = slot.colour;
  if (!colour.empty() && colour[0] == '#') colour = colour.substr(1);
  if (colour.size() >= 6) {
    try {
      *valid = true;
      return lv_color_hex(std::stoul(colour.substr(0, 6), nullptr, 16));
    } catch (const std::exception &) {}
  }
  *valid = false;
  return lv_palette_darken(LV_PALETTE_GREY, 2);
}
void MmuPanel::rebuild_grid() {
  visible_cards.clear();
  lv_obj_clean(cards_row1);
  lv_obj_clean(cards_row2);

  size_t total_lanes = lanes.size();
  size_t total_pages = (total_lanes + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
  bool nav_visible = total_pages > 1;
  size_t start_idx = current_page * CARDS_PER_PAGE;
  size_t end_idx = std::min(start_idx + CARDS_PER_PAGE, total_lanes);
  size_t page_count = end_idx - start_idx;

  bool single_row_mode = page_count <= CARDS_PER_ROW;
  int spool_diam = single_row_mode ? scale_r(60) : scale_r(42);

  // rows flex-grow, so hiding row 2 hands its share of the height to row 1
  if (single_row_mode) {
    lv_obj_add_flag(cards_row2, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(cards_row2, LV_OBJ_FLAG_HIDDEN);
  }

  for (size_t i = start_idx; i < end_idx; i++) {
    bool is_row2 = (i - start_idx) >= CARDS_PER_ROW;
    lv_obj_t *parent_row = is_row2 ? cards_row2 : cards_row1;

    Card card;
    card.cont = lv_obj_create(parent_row);
    lv_obj_set_height(card.cont, LV_PCT(100));
    lv_obj_set_flex_grow(card.cont, 1);
    lv_obj_set_style_radius(card.cont, scale_r(6), 0);
    lv_obj_set_style_bg_color(card.cont, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_bg_color(card.cont, lv_palette_darken(LV_PALETTE_GREY, 3), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card.cont, 1, 0);
    lv_obj_set_style_border_color(card.cont, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_transform_width(card.cont, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(card.cont, -2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card.cont, scale_r(4), 0);
    lv_obj_clear_flag(card.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card.cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card.cont, (void*)(intptr_t)i);
    lv_obj_add_event_cb(card.cont, &MmuPanel::_handle_card, LV_EVENT_CLICKED, this);

    lv_obj_set_flex_flow(card.cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card.cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    card.spool = lv_obj_create(card.cont);
    card.hole = lv_obj_create(card.spool);
    style_spool_icon(card.spool, card.hole, &card.checker, spool_diam);

    // Group the two text lines tightly; SPACE_EVENLY on the card then puts
    // the breathing room above the spool and below the text
    lv_obj_t *text_box = lv_obj_create(card.cont);
    lv_obj_set_size(text_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(text_box, 0, 0);
    lv_obj_set_style_pad_row(text_box, scale_r(1), 0);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    card.title = lv_label_create(text_box);
    lv_label_set_long_mode(card.title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(card.title, "");
    lv_obj_set_style_text_font(card.title, scale_font(14), 0);
    lv_obj_clear_flag(card.title, LV_OBJ_FLAG_CLICKABLE);

    card.material = lv_label_create(text_box);
    lv_label_set_text(card.material, "");
    lv_obj_set_style_text_font(card.material, scale_font(12), 0);
    lv_obj_clear_flag(card.material, LV_OBJ_FLAG_CLICKABLE);

    visible_cards.push_back(card);
  }

  // pad partial rows with invisible spacers so cards keep the same width
  // as a full row (cards flex-grow, spacers absorb the leftover)
  size_t row1_cards = std::min(page_count, CARDS_PER_ROW);
  size_t row2_cards = page_count > CARDS_PER_ROW ? page_count - CARDS_PER_ROW : 0;
  auto fill_row = [](lv_obj_t *row, size_t missing) {
    for (size_t i = 0; i < missing; i++) {
      lv_obj_t *sp = lv_obj_create(row);
      lv_obj_set_height(sp, LV_PCT(100));
      lv_obj_set_flex_grow(sp, 1);
      lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(sp, 0, 0);
      lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    }
  };
  fill_row(cards_row1, CARDS_PER_ROW - row1_cards);
  if (!single_row_mode) fill_row(cards_row2, CARDS_PER_ROW - row2_cards);

  // Update Pagination Row; hide the arrow that has nowhere to go
  if (nav_visible) {
    lv_obj_clear_flag(nav_row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(nav_label, fmt::format("Page {} / {}", current_page + 1, total_pages).c_str());
    if (current_page == 0) lv_obj_add_flag(nav_prev_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(nav_prev_btn, LV_OBJ_FLAG_HIDDEN);
    if (current_page + 1 >= total_pages) lv_obj_add_flag(nav_next_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(nav_next_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(nav_row, LV_OBJ_FLAG_HIDDEN);
  }
}

void MmuPanel::populate() {
  if (cont == NULL) return;

  // lanes can shrink on a klipper reconfig; don't strand the view on an empty page
  size_t total_pages = lanes.empty() ? 1 : (lanes.size() + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
  if (current_page >= total_pages) {
    current_page = total_pages - 1;
    rebuild_grid();
  }

  size_t start_idx = current_page * CARDS_PER_PAGE;
  size_t end_idx = std::min(start_idx + CARDS_PER_PAGE, lanes.size());
  size_t expected_card_count = end_idx > start_idx ? (end_idx - start_idx) : 0;

  if (visible_cards.size() != expected_card_count) {
    rebuild_grid();
  }

  lv_color_t primary = theme_primary();

  // Populate visible cards
  for (size_t c = 0; c < visible_cards.size(); c++) {
    size_t lane_idx = start_idx + c;
    if (lane_idx >= lanes.size()) break;

    const MmuSlot &lane = lanes[lane_idx];
    Card &card = visible_cards[c];

    bool has_filament = lane.prepped || lane.ready || lane.tool_loaded;
    bool color_valid = false;
    lv_color_t color = slot_colour(lane, &color_valid);
    bool backup = is_backup_slot((int)lane_idx);

    paint_spool_icon(card.spool, card.hole, card.checker, color, color_valid, has_filament, lane.tool_loaded, primary);

    // Line 1: Tool / Name (e.g. "T0", "T0 (B)")
    std::string tool_str = lane.map.empty() ? lane.name : lane.map;
    if (backup) tool_str += " (B)";
    lv_label_set_text(card.title, tool_str.c_str());
    lv_obj_set_style_text_color(card.title, lane.tool_loaded ? primary : lv_color_white(), 0);

    // Line 2: Material. A configured material shows even when the slot is
    // physically empty (the translucent spool conveys emptiness); bare slots say "Empty"
    if (has_filament) {
      lv_label_set_text(card.material, lane.material.empty() ? "-" : lane.material.c_str());
    } else {
      lv_label_set_text(card.material, lane.material.empty() ? "Empty" : lane.material.c_str());
    }
    lv_obj_set_style_text_color(card.material, lv_palette_main(LV_PALETTE_GREY), 0);

    lv_obj_set_style_border_color(card.cont, lane.tool_loaded ? primary : lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_border_width(card.cont, lane.tool_loaded ? 2 : 1, 0);
  }

  // Header status & error display. A message without error_state is something
  // the backend reported but is not blocking on -- it can sit there for good,
  // so it can be tapped away locally. A real error is only ever cleared by the
  // backend, so that tap asks it to recover instead.
  if (error_state || (!message.empty() && message != dismissed_message)) {
    lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_label_set_text(status_label, fmt::format("{}{}", message.empty() ? "MMU error" : message,
                                                error_state ? " - Tap to reset"
                                                            : " - Tap to dismiss").c_str());
  } else if (bypass) {
    lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_AMBER, 2), 0);
    lv_label_set_text(status_label, "Bypass Active - Single Spool");
  } else {
    lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    std::string text;
    if (busy) {
      text = fmt::format("{}...", current_state);
    } else if (loaded_idx >= 0 && (size_t)loaded_idx < lanes.size()) {
      const MmuSlot &lane = lanes[loaded_idx];
      std::string desc = lane.material.empty() ? lane.name : lane.material;
      if (!lane.map.empty()) desc = fmt::format("{} - {}", lane.map, desc);
      text = fmt::format("Loaded: {}", desc);
    } else {
      text = "Tap spool to configure / load";
    }
    lv_label_set_text(status_label, text.c_str());
  }

  // keep an open edit screen in sync (lane state, print-state button gating)
  if (edit_lane_idx >= 0) {
    if ((size_t)edit_lane_idx < lanes.size()) {
      if (!draft_dirty) {
        // no local edits in progress: follow changes made elsewhere (an RFID
        // scan, the web UI, another screen). the backend owns these values.
        draft_color = lanes[edit_lane_idx].colour;
        draft_material = lanes[edit_lane_idx].material;
      }
      update_edit_preview();
    } else {
      close_edit(); // lane disappeared on a klipper reconfig
    }
  }
}

void MmuPanel::consume(json &j) {
  if (cont == NULL) return;

  auto &pstat_state = j["/params/0/print_stats/state"_json_pointer];
  const bool mmu_updated = backend != NULL && backend->owns_update(j);

  if (pstat_state.is_null() && !mmu_updated) return;

  std::lock_guard<std::mutex> lock(lv_lock);
  refresh();
  push_loaded_filament();
  populate();
}

void MmuPanel::handle_card(lv_event_t *e) {
  lv_obj_t *card = lv_event_get_current_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(card);
  LOG_TRACE("mmu card {} clicked -> opening full-screen config", idx);
  open_edit(idx);
}

void MmuPanel::handle_page_prev(lv_event_t *e) {
  if (current_page > 0) {
    current_page--;
    rebuild_grid();
    populate();
  }
}

void MmuPanel::handle_page_next(lv_event_t *e) {
  size_t total_pages = (lanes.size() + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
  if (current_page + 1 < total_pages) {
    current_page++;
    rebuild_grid();
    populate();
  }
}

void MmuPanel::handle_status_bar(lv_event_t *e) {
  if (backend == NULL) return;
  if (error_state) {
    backend->reset_failure();
  } else if (!message.empty()) {
    // ask the backend to acknowledge it -- a queued message it never pops
    // would hide every later one -- and stop showing this text meanwhile, so
    // the tap feels immediate whether or not the backend has anything to do.
    backend->dismiss_message();
    dismissed_message = message;
    populate(); // already on the UI thread, which holds lv_lock
  }
}

// =========================================================================
// FULL-SCREEN EDIT PANEL LIFECYCLE
// =========================================================================
void MmuPanel::open_edit(int idx) {
  if (idx < 0 || (size_t)idx >= lanes.size()) return;
  edit_lane_idx = idx;

  const MmuSlot &lane = lanes[idx];
  draft_color = lane.colour;
  draft_material = lane.material;
  draft_dirty = false;

  if (edit_panel_cont != NULL) {
    lv_obj_clear_flag(edit_panel_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(edit_panel_cont);
  }
  update_edit_preview();
}

void MmuPanel::close_edit() {
  edit_lane_idx = -1;
  // popouts belong to the edit screen; never leave one stranded on top
  close_backup_picker();
  close_color_picker();
  close_material_picker();
  if (edit_panel_cont != NULL) {
    lv_obj_add_flag(edit_panel_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(edit_panel_cont);
  }
  populate();
}

void MmuPanel::update_edit_preview() {
  if (edit_lane_idx < 0 || (size_t)edit_lane_idx >= lanes.size()) return;
  const MmuSlot &lane = lanes[edit_lane_idx];

  lv_label_set_text(edit_name_lbl, pretty_lane_name(lane.name).c_str());

  lv_color_t color = lv_palette_darken(LV_PALETTE_GREY, 2);
  bool draft_color_valid = false;
  std::string c_str = draft_color;
  if (!c_str.empty() && c_str[0] == '#') c_str = c_str.substr(1);
  if (c_str.size() >= 6) {
    try {
      color = lv_color_hex(std::stoul(c_str.substr(0, 6), nullptr, 16));
      draft_color_valid = true;
    } catch (...) {}
  }

  bool has_filament = lane.prepped || lane.ready || lane.tool_loaded;
  paint_spool_icon(edit_preview_spool, edit_preview_hole, edit_preview_checker, color,
                   draft_color_valid, has_filament, lane.tool_loaded, theme_primary());

  // Material line: "PLA - 750g" when a spool weight is known, else just
  // "PLA". A bare empty lane shows nothing; the weight belongs to the
  // physical spool, so it never shows without filament present.
  std::string mat_str;
  if (!has_filament && lane.material.empty()) {
    mat_str = "-";
  } else {
    mat_str = draft_material.empty() ? "-" : draft_material;
    // remaining grams only mean something when spoolman tracks the spool
    if (has_filament && spoolman_active && lane.weight > 0) {
      mat_str += fmt::format(" - {}g", lane.weight);
    }
  }
  lv_label_set_text(edit_mat_lbl, mat_str.c_str());

  // Tool assignment is read-only info; it comes from the MMU config
  std::string tool_str = fmt::format("Tool: {}", lane.map.empty() ? "None" : lane.map);
  if (is_backup_slot(edit_lane_idx)) {
    tool_str += " (Backup)";
  }
  lv_label_set_text(edit_tool_lbl, tool_str.c_str());

  lv_label_set_text(edit_status_lbl, fmt::format("Status: {}", slot_status(lane)).c_str());

  // Filament motion is blocked while printing or while the unit is mid-operation.
  // Loading also needs filament physically present in the slot.
  bool blocked = printing || busy;
  set_btn_label(edit_load_btn, lane.tool_loaded ? "Unload" : "Load");
  set_action_btn(edit_load_btn, !blocked && (lane.tool_loaded || has_filament),
                 lane.tool_loaded ? lv_palette_darken(LV_PALETTE_RED, 2) : theme_primary());
  // eject needs filament physically present, and not loaded to the tool
  set_action_btn(edit_eject_btn, !blocked && has_filament && !lane.tool_loaded,
                 lv_palette_darken(LV_PALETTE_GREY, 3));

  // Whether the spool metadata can be edited is the backend's call, not ours:
  // both backends accept colour/material on an empty slot, so presence is the
  // wrong test. See MmuSlot::can_configure.
  bool configurable = lane.can_configure;

  // Backup toggle: the backup slot needs filament and cannot back up itself,
  // and is always allowed off so a stale assignment can be cleared. Being the
  // active spool is not a bar -- neither backend refuses it, and a chain is
  // worth setting up whenever, not only while the slot happens to be idle.
  bool backup = is_backup_slot(edit_lane_idx);
  bool can_toggle = backup || (has_filament && lanes.size() > 1);
  // not gated on `blocked`: this is configuration, not filament motion, and
  // both backends accept it mid-print -- which is exactly when you notice a
  // spool running low and want a fallback
  set_btn_label(edit_backup_btn, backup ? "Backup: On" : "Use as Backup");
  set_action_btn(edit_backup_btn, can_toggle,
                 backup ? theme_primary() : lv_palette_darken(LV_PALETTE_GREY, 3));

  set_action_btn(edit_save_btn, configurable, theme_primary());

  // Update Square Color Swatches active outline
  std::string cur_hex = draft_color;
  if (!cur_hex.empty() && cur_hex[0] == '#') cur_hex = cur_hex.substr(1);
  std::transform(cur_hex.begin(), cur_hex.end(), cur_hex.begin(), ::toupper);

  for (size_t i = 0; i < color_swatch_btns.size(); i++) {
    lv_obj_t *s = color_swatch_btns[i];
    const char *hex = COLOR_PRESETS[i];
    bool active = configurable &&
                  ((i == 0) ? (draft_color.empty() || draft_color == "NONE") : (cur_hex == hex));
    lv_obj_set_style_border_width(s, active ? 2 : 1, 0);
    lv_obj_set_style_border_color(s, active ? lv_color_white() : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    if (configurable) lv_obj_clear_state(s, LV_STATE_DISABLED);
    else lv_obj_add_state(s, LV_STATE_DISABLED);
  }
  if (configurable) lv_obj_clear_state(custom_color_btn, LV_STATE_DISABLED);
  else lv_obj_add_state(custom_color_btn, LV_STATE_DISABLED);

  // Update Material Buttons checked state
  for (size_t i = 0; i < material_btns.size(); i++) {
    lv_obj_t *b = material_btns[i];
    bool active = configurable && (draft_material == materials[i]);
    lv_obj_set_style_bg_color(b, active ? theme_primary() : lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_border_width(b, active ? 1 : 0, 0);
    lv_obj_set_style_border_color(b, active ? lv_color_white() : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    if (configurable) lv_obj_clear_state(b, LV_STATE_DISABLED);
    else lv_obj_add_state(b, LV_STATE_DISABLED);
  }
  if (configurable) lv_obj_clear_state(more_mat_btn, LV_STATE_DISABLED);
  else lv_obj_add_state(more_mat_btn, LV_STATE_DISABLED);
}

// "#rrggbb" / "RRGGBB" / "" -> "RRGGBB" / "", so a draft can be compared with
// whatever spelling the backend reported
static std::string normalise_hex(const std::string &colour) {
  if (colour.empty() || colour == "NONE") return "";
  std::string s = colour[0] == '#' ? colour.substr(1) : colour;
  std::transform(s.begin(), s.end(), s.begin(), ::toupper);
  return s;
}

void MmuPanel::save_edit() {
  if (backend == NULL || edit_lane_idx < 0 || (size_t)edit_lane_idx >= lanes.size()) return;
  const MmuSlot &slot = lanes[edit_lane_idx];

  // Only push what the user actually changed. The backend owns this metadata
  // -- it may come from an RFID scan or be edited elsewhere -- so writing back
  // values we merely displayed would let the panel invent or resurrect data.
  const std::string hex = normalise_hex(draft_color);
  if (hex != normalise_hex(slot.colour)) {
    backend->set_colour(edit_lane_idx, hex);
  }
  if (draft_material != slot.material) {
    backend->set_material(edit_lane_idx, draft_material);
  }

  close_edit();
}

void MmuPanel::handle_edit_action(lv_event_t *e) {
  lv_obj_t *target = lv_event_get_current_target(e);

  if (target == edit_back_btn) {
    close_edit();
    return;
  }
  if (target == edit_save_btn) {
    save_edit();
    return;
  }
  if (backend == NULL || edit_lane_idx < 0 || (size_t)edit_lane_idx >= lanes.size()) return;
  const MmuSlot &lane = lanes[edit_lane_idx];

  if (target == edit_load_btn) {
    if (lane.tool_loaded) {
      backend->unload();
    } else if (loaded_idx < 0) {
      backend->load(edit_lane_idx);
    } else {
      backend->change_tool(edit_lane_idx);
    }
    close_edit();
    return;
  }
  if (target == edit_eject_btn) {
    backend->eject(edit_lane_idx);
    close_edit();
    return;
  }
  if (target == edit_backup_btn) {
    if (is_backup_slot(edit_lane_idx)) {
      // clear whichever slot points at this one
      // NOTE: the panel treats a backup as one-to-one -- assigning this slot
      // as a backup clears any other slot already using it. Neither backend
      // requires that (AFC keeps a runout pointer per lane, Happy Hare's
      // endless spool groups are sets), it keeps this screen to a simple
      // on/off toggle. Deliberate: quick config here, anything richer belongs
      // in a dedicated screen.
      for (size_t i = 0; i < lanes.size(); i++) {
        if ((int)i != edit_lane_idx && lanes[i].backup == edit_lane_idx) {
          backend->set_backup((int)i, -1);
        }
      }
    } else {
      open_backup_picker(); // choose which slot this one backs up
    }
    return; // stays open; state comes back via the subscription
  }

  if (target == backup_picker) { // tap outside the popout cancels
    close_backup_picker();
    return;
  }

  for (size_t i = 0; i < backup_pick_btns.size(); i++) {
    if (target == backup_pick_btns[i]) {
      int idx = (int)(intptr_t)lv_obj_get_user_data(target);
      if (idx >= 0 && (size_t)idx < lanes.size()) {
        // move any existing pointer to this backup before assigning the new
        // one; see the one-to-one note on the toggle above
        for (size_t i = 0; i < lanes.size(); i++) {
          if ((int)i != edit_lane_idx && lanes[i].backup == edit_lane_idx) {
            backend->set_backup((int)i, -1);
          }
        }
        backend->set_backup(idx, edit_lane_idx);
      }
      close_backup_picker();
      return;
    }
  }

  if (target == custom_color_btn) {
    open_color_picker();
    return;
  }
  if (target == color_picker || target == color_pick_cancel) {
    close_color_picker();
    return;
  }
  if (target == color_wheel || target == color_sat_slider || target == color_val_slider) {
    lv_obj_set_style_bg_color(color_pick_preview, picker_color(), 0);
    return;
  }
  if (target == color_pick_ok) {
    lv_color32_t c32;
    c32.full = lv_color_to32(picker_color());
    draft_color = fmt::format("{:02X}{:02X}{:02X}", c32.ch.red, c32.ch.green, c32.ch.blue);
    draft_dirty = true;
    close_color_picker();
    update_edit_preview();
    return;
  }

  if (target == more_mat_btn) {
    open_material_picker();
    return;
  }
  if (target == material_picker) {
    close_material_picker();
    return;
  }
  for (size_t i = 0; i < mat_pick_btns.size(); i++) {
    if (target == mat_pick_btns[i]) {
      const char *mat = (const char*)lv_obj_get_user_data(target);
      if (mat != NULL) {
        draft_material = mat;
        draft_dirty = true;
      }
      close_material_picker();
      update_edit_preview();
      return;
    }
  }

  // Check if target is a square color swatch
  for (size_t i = 0; i < color_swatch_btns.size(); i++) {
    if (target == color_swatch_btns[i]) {
      const char *hex = (const char*)lv_obj_get_user_data(target);
      draft_color = hex ? hex : "";
      draft_dirty = true;
      update_edit_preview();
      return;
    }
  }

  // Check if target is a material preset
  for (size_t i = 0; i < material_btns.size(); i++) {
    if (target == material_btns[i]) {
      const char *mat = (const char*)lv_obj_get_user_data(target);
      draft_material = mat ? mat : "";
      draft_dirty = true;
      update_edit_preview();
      return;
    }
  }
}
// =========================================================================
// BACKUP PICKER: choose which lane the edited lane backs up
// =========================================================================
void MmuPanel::open_backup_picker() {
  if (edit_lane_idx < 0 || (size_t)edit_lane_idx >= lanes.size()) return;
  const MmuSlot &editing = lanes[edit_lane_idx];

  if (backup_picker == NULL) {
    backup_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(backup_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(backup_picker, 0, 0);
    lv_obj_set_style_bg_color(backup_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backup_picker, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backup_picker, 0, 0);
    lv_obj_clear_flag(backup_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backup_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backup_picker, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);

    backup_picker_list = lv_obj_create(backup_picker);
    lv_obj_set_style_radius(backup_picker_list, scale_r(8), 0);
    lv_obj_set_style_bg_color(backup_picker_list, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(backup_picker_list, 1, 0);
    lv_obj_set_style_border_color(backup_picker_list, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_pad_all(backup_picker_list, scale_r(10), 0);
    lv_obj_set_style_pad_row(backup_picker_list, scale_r(6), 0);
    lv_obj_set_style_pad_column(backup_picker_list, scale_r(6), 0);
    lv_obj_set_flex_flow(backup_picker_list, LV_FLEX_FLOW_ROW_WRAP);
  }

  lv_obj_clean(backup_picker_list);
  backup_pick_btns.clear();

  // size the popout to the lane count: mini spool tiles fill the row
  // (grow doesn't wrap in lv_flex, so compute the tile width instead).
  // the box hugs its content; past the cap it scrolls
  int n = lanes.size() > 0 ? (int)lanes.size() - 1 : 0;
  int cols = std::max(1, std::min(n, 4));
  int box_w = popout_w();
  int tile_w = (popout_row_w() - (cols - 1) * scale_r(6)) / cols;
  lv_obj_set_width(backup_picker_list, box_w);
  lv_obj_set_height(backup_picker_list, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(backup_picker_list, popout_max_h(), 0);
  lv_obj_center(backup_picker_list);

  lv_obj_t *title = lv_label_create(backup_picker_list);
  lv_label_set_text(title, fmt::format("{} backs up:", pretty_lane_name(editing.name)).c_str());
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_font(title, scale_font(14), 0);

  for (size_t i = 0; i < lanes.size(); i++) {
    const MmuSlot &l = lanes[i];
    if (l.name == editing.name) continue;

    lv_obj_t *b = lv_btn_create(backup_picker_list);
    lv_obj_set_size(b, tile_w, scale_h(74));
    lv_obj_set_style_radius(b, scale_r(6), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREY, 2), LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(b, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, -2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, scale_r(4), 0);
    lv_obj_set_style_pad_row(b, scale_r(2), 0);
    lv_obj_set_user_data(b, (void*)(intptr_t)i);
    lv_obj_add_event_cb(b, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // mini spool mirrors the lane's color; translucent when the slot is empty
    bool color_valid = false;
    lv_color_t c = slot_colour(l, &color_valid);
    bool has_filament = l.prepped || l.ready || l.tool_loaded;
    lv_obj_t *dot = lv_obj_create(b);
    lv_obj_set_size(dot, scale_r(30), scale_r(30));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, c, 0);
    lv_obj_set_style_bg_opa(dot, has_filament && color_valid ? LV_OPA_COVER
                                 : color_valid ? LV_OPA_50 : LV_OPA_20, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, l.tool_loaded ? theme_primary()
                                       : (color_valid && lv_color_brightness(c) < 60)
                                         ? lv_palette_main(LV_PALETTE_GREY)
                                         : lv_palette_darken(LV_PALETTE_GREY, 1), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    std::string name = l.map.empty() ? pretty_lane_name(l.name) : l.map;
    if (l.tool_loaded) name += " *";
    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, name.c_str());
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl, scale_font(12), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mat = lv_label_create(b);
    lv_label_set_text(mat, l.material.empty() ? "Empty" : l.material.c_str());
    lv_obj_set_style_text_font(mat, scale_font(12), 0);
    lv_obj_set_style_text_color(mat, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_clear_flag(mat, LV_OBJ_FLAG_CLICKABLE);

    backup_pick_btns.push_back(b);
  }

  lv_obj_t *cancel = create_flat_btn(backup_picker_list, "Cancel",
                                     &MmuPanel::_handle_edit_action, this);
  lv_obj_set_size(cancel, LV_PCT(100), scale_h(32));
  lv_obj_set_style_radius(cancel, scale_r(4), 0);
  lv_obj_set_style_bg_color(cancel, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_set_user_data(cancel, (void*)(intptr_t)-1);
  backup_pick_btns.push_back(cancel);

  lv_obj_scroll_to_y(backup_picker_list, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(backup_picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(backup_picker);
}

void MmuPanel::close_backup_picker() {
  if (backup_picker != NULL) {
    lv_obj_add_flag(backup_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(backup_picker);
  }
}

// =========================================================================
// CUSTOM COLOR PICKER (colorwheel popout)
// =========================================================================
void MmuPanel::open_color_picker() {
  if (color_picker == NULL) {
    color_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(color_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(color_picker, 0, 0);
    lv_obj_set_style_bg_color(color_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(color_picker, LV_OPA_50, 0);
    lv_obj_set_style_border_width(color_picker, 0, 0);
    lv_obj_clear_flag(color_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(color_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(color_picker, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);

    lv_obj_t *box = lv_obj_create(color_picker);
    lv_obj_set_size(box, popout_w(), popout_max_h());
    lv_obj_center(box);
    lv_obj_set_style_radius(box, scale_r(8), 0);
    lv_obj_set_style_bg_color(box, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_pad_all(box, scale_r(12), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // big hue wheel on the left; less fiddly to grab
    color_wheel = lv_colorwheel_create(box, true);
    lv_obj_set_size(color_wheel, scale_r(200), scale_r(200));
    // ring thickness keeps the designed 22px-per-200px-wheel proportion at
    // any resolution (the theme's DPI-derived default barely grows)
    lv_obj_set_style_arc_width(color_wheel, scale_r(22), LV_PART_MAIN);
    lv_obj_align(color_wheel, LV_ALIGN_LEFT_MID, scale_w(4), 0);
    lv_obj_add_event_cb(color_wheel, &MmuPanel::_handle_edit_action, LV_EVENT_VALUE_CHANGED, this);

    // right side: preview, saturation, brightness, save/cancel
    lv_obj_t *right = lv_obj_create(box);
    lv_obj_set_size(right, scale_w(200), LV_PCT(100));
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_row(right, scale_r(6), 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);

    color_pick_preview = lv_obj_create(right);
    lv_obj_set_size(color_pick_preview, LV_PCT(100), scale_h(40));
    lv_obj_set_style_radius(color_pick_preview, scale_r(4), 0);
    lv_obj_set_style_border_width(color_pick_preview, 1, 0);
    lv_obj_set_style_border_color(color_pick_preview, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_clear_flag(color_pick_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(color_pick_preview, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sat_lbl = lv_label_create(right);
    lv_label_set_text(sat_lbl, "SATURATION:");
    lv_obj_set_style_text_font(sat_lbl, scale_font(12), 0);
    lv_obj_set_style_text_color(sat_lbl, lv_palette_main(LV_PALETTE_GREY), 0);

    color_sat_slider = lv_slider_create(right);
    lv_obj_set_size(color_sat_slider, LV_PCT(96), scale_h(12));
    lv_slider_set_range(color_sat_slider, 0, 100);
    lv_slider_set_value(color_sat_slider, 100, LV_ANIM_OFF);
    lv_obj_add_event_cb(color_sat_slider, &MmuPanel::_handle_edit_action, LV_EVENT_VALUE_CHANGED, this);

    // breathing room between the two sliders
    lv_obj_t *slider_gap = lv_obj_create(right);
    lv_obj_set_size(slider_gap, LV_PCT(100), scale_h(8));
    lv_obj_set_style_bg_opa(slider_gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(slider_gap, 0, 0);
    lv_obj_clear_flag(slider_gap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(slider_gap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *val_lbl = lv_label_create(right);
    lv_label_set_text(val_lbl, "BRIGHTNESS:");
    lv_obj_set_style_text_font(val_lbl, scale_font(12), 0);
    lv_obj_set_style_text_color(val_lbl, lv_palette_main(LV_PALETTE_GREY), 0);

    color_val_slider = lv_slider_create(right);
    lv_obj_set_size(color_val_slider, LV_PCT(96), scale_h(12));
    lv_slider_set_range(color_val_slider, 0, 100);
    lv_slider_set_value(color_val_slider, 100, LV_ANIM_OFF);
    lv_obj_add_event_cb(color_val_slider, &MmuPanel::_handle_edit_action, LV_EVENT_VALUE_CHANGED, this);

    // spacer pushes the buttons to the bottom, away from the sliders
    lv_obj_t *btn_spacer = lv_obj_create(right);
    lv_obj_set_width(btn_spacer, LV_PCT(100));
    lv_obj_set_flex_grow(btn_spacer, 1);
    lv_obj_set_style_bg_opa(btn_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_spacer, 0, 0);
    lv_obj_clear_flag(btn_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_spacer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *btn_row = lv_obj_create(right);
    lv_obj_set_size(btn_row, LV_PCT(100), scale_h(46));
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, scale_r(8), 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);

    color_pick_ok = create_flat_btn(btn_row, "Save", &MmuPanel::_handle_edit_action, this);
    lv_obj_set_height(color_pick_ok, LV_PCT(100));
    lv_obj_set_flex_grow(color_pick_ok, 1);
    lv_obj_set_style_radius(color_pick_ok, scale_r(4), 0);
    lv_obj_set_style_bg_color(color_pick_ok, theme_primary(), 0);

    color_pick_cancel = create_flat_btn(btn_row, "Cancel", &MmuPanel::_handle_edit_action, this);
    lv_obj_set_height(color_pick_cancel, LV_PCT(100));
    lv_obj_set_flex_grow(color_pick_cancel, 1);
    lv_obj_set_style_radius(color_pick_cancel, scale_r(4), 0);
    lv_obj_set_style_bg_color(color_pick_cancel, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  }

  // seed wheel + sliders from the current draft color
  lv_color_t seed = lv_palette_main(LV_PALETTE_RED);
  std::string c_str = draft_color;
  if (!c_str.empty() && c_str[0] == '#') c_str = c_str.substr(1);
  if (c_str.size() >= 6) {
    try { seed = lv_color_hex(std::stoul(c_str.substr(0, 6), nullptr, 16)); } catch (...) {}
  }
  lv_color32_t s32;
  s32.full = lv_color_to32(seed);
  lv_color_hsv_t hsv = lv_color_rgb_to_hsv(s32.ch.red, s32.ch.green, s32.ch.blue);
  lv_colorwheel_set_hsv(color_wheel, {hsv.h, 100, 100});
  lv_slider_set_value(color_sat_slider, hsv.s, LV_ANIM_OFF);
  lv_slider_set_value(color_val_slider, hsv.v, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(color_pick_preview, seed, 0);

  lv_obj_clear_flag(color_picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(color_picker);
}

// hue from the wheel, saturation/brightness from the sliders
lv_color_t MmuPanel::picker_color() {
  lv_color_hsv_t hsv = lv_colorwheel_get_hsv(color_wheel);
  return lv_color_hsv_to_rgb(hsv.h,
                             (uint8_t)lv_slider_get_value(color_sat_slider),
                             (uint8_t)lv_slider_get_value(color_val_slider));
}

void MmuPanel::close_color_picker() {
  if (color_picker != NULL) {
    lv_obj_add_flag(color_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(color_picker);
  }
}

// =========================================================================
// MATERIAL PICKER (catalog popout)
// =========================================================================
void MmuPanel::open_material_picker() {
  if (material_picker == NULL) {
    material_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(material_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(material_picker, 0, 0);
    lv_obj_set_style_bg_color(material_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(material_picker, LV_OPA_50, 0);
    lv_obj_set_style_border_width(material_picker, 0, 0);
    lv_obj_clear_flag(material_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(material_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(material_picker, &MmuPanel::_handle_edit_action, LV_EVENT_CLICKED, this);

    material_picker_list = lv_obj_create(material_picker);
    lv_obj_set_width(material_picker_list, popout_w());
    lv_obj_set_height(material_picker_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(material_picker_list, popout_max_h(), 0);
    lv_obj_center(material_picker_list);
    lv_obj_set_style_radius(material_picker_list, scale_r(8), 0);
    lv_obj_set_style_bg_color(material_picker_list, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(material_picker_list, 1, 0);
    lv_obj_set_style_border_color(material_picker_list, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_pad_all(material_picker_list, scale_r(10), 0);
    lv_obj_set_style_pad_row(material_picker_list, scale_r(6), 0);
    lv_obj_set_style_pad_column(material_picker_list, scale_r(6), 0);
    lv_obj_set_flex_flow(material_picker_list, LV_FLEX_FLOW_ROW_WRAP);

    lv_obj_t *title = lv_label_create(material_picker_list);
    lv_label_set_text(title, "Material:");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, scale_font(14), 0);

    mat_pick_btns.clear();
    const int cols = 4;
    const int chip_w = (popout_row_w() - (cols - 1) * scale_r(6)) / cols;
    for (size_t i = 0; i < sizeof(MATERIAL_CATALOG) / sizeof(MATERIAL_CATALOG[0]); i++) {
      lv_obj_t *b = create_flat_btn(material_picker_list, MATERIAL_CATALOG[i],
                                    &MmuPanel::_handle_edit_action, this);
      lv_obj_set_size(b, chip_w, scale_h(38));
      lv_obj_set_style_radius(b, scale_r(4), 0);
      lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
      lv_obj_set_user_data(b, (void*)MATERIAL_CATALOG[i]);
      mat_pick_btns.push_back(b);
    }

    lv_obj_t *cancel = create_flat_btn(material_picker_list, "Cancel",
                                       &MmuPanel::_handle_edit_action, this);
    lv_obj_set_size(cancel, LV_PCT(100), scale_h(32));
    lv_obj_set_style_radius(cancel, scale_r(4), 0);
    lv_obj_set_style_bg_color(cancel, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_user_data(cancel, (void*)NULL);
    mat_pick_btns.push_back(cancel);
  }

  // highlight the currently selected material
  for (auto *b : mat_pick_btns) {
    const char *mat = (const char*)lv_obj_get_user_data(b);
    bool active = mat != NULL && draft_material == mat;
    lv_obj_set_style_bg_color(b, active ? theme_primary()
                              : lv_palette_darken(LV_PALETTE_GREY, mat == NULL ? 2 : 3), 0);
  }

  lv_obj_scroll_to_y(material_picker_list, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(material_picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(material_picker);
}

void MmuPanel::close_material_picker() {
  if (material_picker != NULL) {
    lv_obj_add_flag(material_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(material_picker);
  }
}
