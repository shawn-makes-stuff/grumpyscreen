#include "afc_panel.h"
#include "state.h"
#include "utils.h"
#include "logger.h"

LV_IMG_DECLARE(back);
LV_IMG_DECLARE(refresh_img);
LV_IMG_DECLARE(unload_filament_img);

static const int COL_NAME = 0;
static const int COL_MATERIAL = 1;
static const int COL_COLOR = 2;
static const int COL_STATUS = 3;
static const int COL_LOAD = 4;
static const int COL_EJECT = 5;

AfcPanel::AfcPanel(KWebSocketClient &c, std::mutex &l)
  : NotifyConsumer(l)
  , ws(c)
  , cont(lv_obj_create(lv_scr_act()))
  , status_label(lv_label_create(cont))
  , lane_table(lv_table_create(cont))
  , controls(lv_obj_create(cont))
  , unload_btn(controls, &unload_filament_img, "Unload", &AfcPanel::_handle_callback, this)
  , reset_btn(controls, &refresh_img, "Reset", &AfcPanel::_handle_callback, this)
  , back_btn(controls, &back, "Back", &AfcPanel::_handle_callback, this)
  , error_state(false)
  , bypass(false)
  , printing(false)
{
  lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(cont);

  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

  lv_obj_set_width(status_label, LV_PCT(100));
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
  lv_label_set_text(status_label, "");
  lv_obj_set_style_pad_left(status_label, 5, 0);
  lv_obj_set_style_pad_top(status_label, 5, 0);

  lv_obj_set_width(lane_table, LV_PCT(100));
  lv_obj_set_flex_grow(lane_table, 1);

  lv_table_set_col_cnt(lane_table, 6);
  auto screen_width = lv_disp_get_physical_hor_res(NULL);
  auto scale = (double)screen_width / 800.0;
  lv_table_set_col_width(lane_table, COL_MATERIAL, 110 * scale);
  lv_table_set_col_width(lane_table, COL_COLOR, 50 * scale);
  lv_table_set_col_width(lane_table, COL_STATUS, 140 * scale);
  lv_table_set_col_width(lane_table, COL_LOAD, 70 * scale);
  lv_table_set_col_width(lane_table, COL_EJECT, 70 * scale);
  lv_table_set_col_width(lane_table, COL_NAME,
			 screen_width - scale * (110 + 50 + 140 + 70 + 70));

  // controls
  lv_obj_set_width(controls, LV_PCT(100));
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END);

  lv_obj_add_event_cb(lane_table, &AfcPanel::_handle_table_action, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(lane_table, &AfcPanel::_handle_table_action, LV_EVENT_DRAW_PART_BEGIN, this);

  ws.register_notify_update(this);
}

AfcPanel::~AfcPanel() {
  if (cont != NULL) {
    lv_obj_del(cont);
    cont = NULL;
  }
}

void AfcPanel::foreground() {
  refresh();
  populate();
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cont);
}

void AfcPanel::refresh() {
  State *state = State::get_instance();
  json &afc = state->get_data("/printer_state/AFC"_json_pointer);

  lanes.clear();
  current_load = "";
  current_state = "";
  message = "";
  error_state = false;
  bypass = false;

  if (afc.is_null()) {
    return;
  }

  auto &load = afc["/current_load"_json_pointer];
  if (!load.is_null()) {
    current_load = load.template get<std::string>();
  }

  auto &cur_state = afc["/current_state"_json_pointer];
  if (!cur_state.is_null()) {
    current_state = cur_state.template get<std::string>();
  }

  auto &msg = afc["/message/message"_json_pointer];
  if (!msg.is_null()) {
    message = msg.template get<std::string>();
  }

  auto &err = afc["/error_state"_json_pointer];
  if (!err.is_null()) {
    error_state = err.template get<bool>();
  }

  auto &byp = afc["/bypass_state"_json_pointer];
  if (!byp.is_null()) {
    bypass = byp.template get<bool>();
  }

  auto &lane_names = afc["/lanes"_json_pointer];
  if (lane_names.is_null()) {
    return;
  }

  json &objects = state->get_data("/printer_objs/objects"_json_pointer);
  for (auto &l : lane_names) {
    const std::string lane_name = l.template get<std::string>();

    // lanes are registered as "AFC_<unit_type> <lane_name>", e.g.
    // "AFC_stepper lane1" or "AFC_canvas_lane CANVAS_1"
    std::string obj_key;
    if (!objects.is_null()) {
      for (auto &o : objects) {
	const std::string obj_name = o.template get<std::string>();
	if (obj_name.rfind("AFC_", 0) != 0) {
	  continue;
	}
	auto space = obj_name.find(' ');
	if (space != std::string::npos && obj_name.substr(space + 1) == lane_name) {
	  json &st = state->get_data(json::json_pointer(fmt::format("/printer_state/{}", obj_name)));
	  if (!st.is_null() && st.contains("load")) {
	    obj_key = obj_name;
	    break;
	  }
	}
      }
    }

    if (obj_key.empty()) {
      LOG_DEBUG("no status object found for afc lane {}", lane_name);
      continue;
    }

    json &st = state->get_data(json::json_pointer(fmt::format("/printer_state/{}", obj_key)));
    Lane lane;
    lane.name = lane_name;
    if (st.contains("map") && !st["map"].is_null()) {
      lane.map = st["map"].template get<std::string>();
    }
    if (st.contains("material") && !st["material"].is_null()) {
      lane.material = st["material"].template get<std::string>();
    }
    if (st.contains("color") && !st["color"].is_null()) {
      lane.color = st["color"].template get<std::string>();
    }
    if (st.contains("prep") && st["prep"].is_boolean()) {
      lane.prep = st["prep"].template get<bool>();
    }
    if (st.contains("load") && st["load"].is_boolean()) {
      lane.load = st["load"].template get<bool>();
    }
    if (st.contains("tool_loaded") && st["tool_loaded"].is_boolean()) {
      lane.tool_loaded = st["tool_loaded"].template get<bool>();
    }
    if (st.contains("loaded_to_hub") && st["loaded_to_hub"].is_boolean()) {
      lane.loaded_to_hub = st["loaded_to_hub"].template get<bool>();
    }

    lanes.push_back(lane);
  }
}

void AfcPanel::populate() {
  lv_table_set_cell_value(lane_table, 0, COL_NAME, "Lane");
  lv_table_set_cell_value(lane_table, 0, COL_MATERIAL, "MAT");
  lv_table_set_cell_value(lane_table, 0, COL_COLOR, "");
  lv_table_set_cell_value(lane_table, 0, COL_STATUS, "Status");
  lv_table_set_cell_value(lane_table, 0, COL_LOAD, "Load");
  lv_table_set_cell_value(lane_table, 0, COL_EJECT, "Eject");

  size_t row_idx = 1;
  for (auto &lane : lanes) {
    const std::string display_name = lane.map.empty()
      ? lane.name
      : fmt::format("{} ({})", lane.name, lane.map);
    lv_table_set_cell_value(lane_table, row_idx, COL_NAME, display_name.c_str());
    lv_table_set_cell_value(lane_table, row_idx, COL_MATERIAL,
			    lane.material.empty() ? "-" : lane.material.c_str());
    lv_table_set_cell_value(lane_table, row_idx, COL_COLOR, "");

    const char *status = "Empty";
    if (lane.tool_loaded) {
      status = "Loaded";
    } else if (lane.loaded_to_hub) {
      status = "In Hub";
    } else if (lane.prep && lane.load) {
      status = "Ready";
    } else if (lane.prep || lane.load) {
      status = "Inserted";
    }
    lv_table_set_cell_value(lane_table, row_idx, COL_STATUS, status);

    bool can_load = lane.prep && lane.load && !lane.tool_loaded && !printing;
    bool can_eject = (lane.prep || lane.load) && !lane.tool_loaded && !printing;
    lv_table_set_cell_value(lane_table, row_idx, COL_LOAD, can_load ? LV_SYMBOL_PLAY : "");
    lv_table_set_cell_value(lane_table, row_idx, COL_EJECT, can_eject ? LV_SYMBOL_EJECT : "");
    row_idx++;
  }
  lv_table_set_row_cnt(lane_table, row_idx);

  if (!message.empty()) {
    lv_label_set_text(status_label, message.c_str());
  } else {
    lv_label_set_text(status_label,
		      fmt::format("State: {} | Loaded: {}{}",
				  current_state.empty() ? "unknown" : current_state,
				  current_load.empty() ? "none" : current_load,
				  bypass ? " | Bypass" : "").c_str());
  }

  if (!current_load.empty() && !printing) {
    unload_btn.enable();
  } else {
    unload_btn.disable();
  }

  if (error_state) {
    reset_btn.enable();
  } else {
    reset_btn.disable();
  }
}

void AfcPanel::consume(json &j) {
  auto &pstat_state = j["/params/0/print_stats/state"_json_pointer];
  bool afc_updated = false;

  auto &status = j["/params/0"_json_pointer];
  if (status.is_object()) {
    for (auto &el : status.items()) {
      if (el.key().rfind("AFC", 0) == 0) {
	afc_updated = true;
	break;
      }
    }
  }

  if (!pstat_state.is_null() || afc_updated) {
    std::lock_guard<std::mutex> lock(lv_lock);
    if (!pstat_state.is_null()) {
      printing = pstat_state.template get<std::string>() == "printing";
      if (printing) {
	lv_obj_move_background(cont);
      }
    }

    if (afc_updated || !pstat_state.is_null()) {
      refresh();
      populate();
    }
  }
}

void AfcPanel::handle_callback(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_obj_t *btn = lv_event_get_current_target(e);

    if (btn == back_btn.get_container()) {
      lv_obj_move_background(cont);
    } else if (btn == unload_btn.get_container()) {
      if (!unload_btn.start_pressed_transition(2000)) {
	return;
      }
      LOG_TRACE("afc unload tool");
      ws.gcode_script("TOOL_UNLOAD");
    } else if (btn == reset_btn.get_container()) {
      if (!reset_btn.start_pressed_transition(2000)) {
	return;
      }
      LOG_TRACE("afc reset failure");
      ws.gcode_script("RESET_FAILURE");
    }
  }
}

void AfcPanel::handle_table_action(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    uint16_t row;
    uint16_t col;

    lv_table_get_selected_cell(lane_table, &row, &col);
    uint16_t row_count = lv_table_get_row_cnt(lane_table);
    if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE
	|| row == 0 || row >= row_count || (size_t)(row - 1) >= lanes.size()) {
      return;
    }

    const Lane &lane = lanes[row - 1];
    const char *selected = lv_table_get_cell_value(lane_table, row, col);
    if (selected == NULL || strlen(selected) == 0) {
      return;
    }

    if (col == COL_LOAD && std::memcmp(LV_SYMBOL_PLAY, selected, 3) == 0) {
      if (current_load.empty()) {
	LOG_TRACE("afc load lane {}", lane.name);
	ws.gcode_script(fmt::format("TOOL_LOAD LANE={}", lane.name));
      } else {
	LOG_TRACE("afc change tool to lane {}", lane.name);
	ws.gcode_script(fmt::format("CHANGE_TOOL LANE={}", lane.name));
      }
    } else if (col == COL_EJECT && std::memcmp(LV_SYMBOL_EJECT, selected, 3) == 0) {
      LOG_TRACE("afc eject lane {}", lane.name);
      ws.gcode_script(fmt::format("LANE_UNLOAD LANE={}", lane.name));
    }
  } else if (code == LV_EVENT_DRAW_PART_BEGIN) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part == LV_PART_ITEMS) {
      uint32_t row = dsc->id / lv_table_get_col_cnt(lane_table);
      uint32_t col = dsc->id - row * lv_table_get_col_cnt(lane_table);

      if (row == 0) {
	dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
	dsc->rect_dsc->bg_color = lv_color_mix(lv_palette_main(LV_PALETTE_BLUE),
					       dsc->rect_dsc->bg_color, LV_OPA_20);
	dsc->rect_dsc->bg_opa = LV_OPA_COVER;
	return;
      }

      if (col == COL_COLOR && (size_t)(row - 1) < lanes.size()) {
	// lane colors are "#RRGGBB" or "#RRGGBBAA"
	std::string color = lanes[row - 1].color;
	if (!color.empty() && color[0] == '#') {
	  color = color.substr(1);
	}
	if (color.size() >= 6) {
	  try {
	    dsc->rect_dsc->bg_color = lv_color_hex(std::stoul(color.substr(0, 6), nullptr, 16));
	    dsc->rect_dsc->bg_opa = LV_OPA_COVER;
	  } catch (const std::exception &) {
	    // unparsable color, leave the cell alone
	  }
	}
	return;
      }

      if ((size_t)(row - 1) < lanes.size() && lanes[row - 1].tool_loaded) {
	dsc->rect_dsc->bg_color = lv_color_mix(lv_palette_main(LV_PALETTE_GREEN),
					       dsc->rect_dsc->bg_color, LV_OPA_20);
	dsc->rect_dsc->bg_opa = LV_OPA_COVER;
      } else if ((row % 2) == 0) {
	dsc->rect_dsc->bg_color = lv_color_mix(lv_palette_main(LV_PALETTE_GREY),
					       dsc->rect_dsc->bg_color, LV_OPA_10);
	dsc->rect_dsc->bg_opa = LV_OPA_COVER;
      }
    }
  }
}
