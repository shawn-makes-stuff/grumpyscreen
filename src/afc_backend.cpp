#include "afc_backend.h"
#include "state.h"
#include "logger.h"

#include <algorithm>

bool AfcBackend::detect() {
  json &objs = State::get_instance()->get_data("/printer_objs/objects"_json_pointer);
  if (!objs.is_array()) return false;
  return std::any_of(objs.begin(), objs.end(), [](const json &o) {
    return o.is_string() && o.template get<std::string>() == "AFC";
  });
}

bool AfcBackend::owns_update(json &j) {
  auto &status = j["/params/0"_json_pointer];
  if (!status.is_object()) return false;
  for (auto &el : status.items()) {
    if (el.key().rfind("AFC", 0) == 0) return true;
  }
  return false;
}

void AfcBackend::refresh() {
  State *state = State::get_instance();
  json &afc = state->get_data("/printer_state/AFC"_json_pointer);

  slots.clear();
  lane_ids.clear();
  loaded_slot = -1;
  status_text = "";
  message = "";
  error = false;
  bypass = false;
  busy = false;

  if (afc.is_null()) return;

  std::string current_load;
  auto &load_j = afc["/current_load"_json_pointer];
  if (!load_j.is_null()) current_load = load_j.template get<std::string>();

  auto &cur_state = afc["/current_state"_json_pointer];
  if (!cur_state.is_null()) status_text = cur_state.template get<std::string>();

  auto &msg = afc["/message/message"_json_pointer];
  if (!msg.is_null()) message = msg.template get<std::string>();

  auto &err = afc["/error_state"_json_pointer];
  if (!err.is_null()) error = err.template get<bool>();

  auto &byp = afc["/bypass_state"_json_pointer];
  if (!byp.is_null()) bypass = byp.template get<bool>();

  // AFC reports spoolman as a bool (or a URL string in some versions)
  auto &spm = afc["/spoolman"_json_pointer];
  spoolman = (spm.is_boolean() && spm.template get<bool>()) ||
             (spm.is_string() && !spm.template get<std::string>().empty());

  std::vector<std::string> runout; // per-slot runout lane name, resolved below
  auto &lane_names = afc["/lanes"_json_pointer];
  if (!lane_names.is_null()) {
    json &objects = state->get_data("/printer_objs/objects"_json_pointer);
    for (auto &l : lane_names) {
      const std::string lane_name = l.template get<std::string>();
      std::string obj_key;
      if (!objects.is_null()) {
        for (auto &o : objects) {
          const std::string obj_name = o.template get<std::string>();
          if (obj_name.rfind("AFC_", 0) != 0) continue;
          auto space = obj_name.find(' ');
          if (space != std::string::npos && obj_name.substr(space + 1) == lane_name) {
            json &st = state->get_data(json::json_pointer(fmt::format("/printer_state/{}", obj_name)));
            if (!st.is_null() && (st.contains("load") || st.contains("prep"))) {
              obj_key = obj_name;
              break;
            }
          }
        }
      }

      if (obj_key.empty()) continue;

      json &st = state->get_data(json::json_pointer(fmt::format("/printer_state/{}", obj_key)));
      MmuSlot slot;
      // "lane12" reads awkwardly as a title; show "Lane 12". Custom names,
      // which AFC also allows, pass through untouched.
      slot.name = lane_name;
      if (lane_name.rfind("lane", 0) == 0 && lane_name.size() > 4 &&
          std::all_of(lane_name.begin() + 4, lane_name.end(),
                      [](unsigned char c) { return std::isdigit(c); })) {
        slot.name = fmt::format("Lane {}", lane_name.substr(4));
      }
      std::string runout_lane;
      // AFC published `map` as a string until 1.2.x, then as a list of the
      // T(n) macros mapped to the lane. Accept both.
      if (st.contains("map")) {
        const json &m = st["map"];
        if (m.is_string()) {
          slot.map = m.template get<std::string>();
        } else if (m.is_array()) {
          for (const auto &t : m) {
            if (!t.is_string()) continue;
            if (!slot.map.empty()) slot.map += ",";
            slot.map += t.template get<std::string>();
          }
        }
      }
      if (st.contains("runout_lane") && st["runout_lane"].is_string()) runout_lane = st["runout_lane"].template get<std::string>();
      if (st.contains("material") && !st["material"].is_null()) slot.material = st["material"].template get<std::string>();
      if (st.contains("color") && st["color"].is_string()) {
        slot.colour = st["color"].template get<std::string>();
        if (!slot.colour.empty() && slot.colour[0] == '#') slot.colour.erase(0, 1);
      }
      if (st.contains("prep") && st["prep"].is_boolean()) slot.prepped = st["prep"].template get<bool>();
      bool fed = false, hub = false;
      if (st.contains("load") && st["load"].is_boolean()) fed = st["load"].template get<bool>();
      if (st.contains("loaded_to_hub") && st["loaded_to_hub"].is_boolean()) hub = st["loaded_to_hub"].template get<bool>();
      slot.ready = fed || hub;
      if (st.contains("tool_loaded") && st["tool_loaded"].is_boolean()) slot.tool_loaded = st["tool_loaded"].template get<bool>();
      if (st.contains("weight") && st["weight"].is_number()) slot.weight = st["weight"].template get<int>();
      if (st.contains("spool_id") && st["spool_id"].is_number()) slot.spool_id = st["spool_id"].template get<int>();

      slots.push_back(slot);
      lane_ids.push_back(lane_name);
      runout.push_back(runout_lane);
    }
  }

  for (size_t i = 0; i < slots.size(); i++) {
    if (lane_ids[i] == current_load) loaded_slot = (int)i;
    if (runout[i].empty()) continue;
    for (size_t b = 0; b < slots.size(); b++) {
      if (lane_ids[b] == runout[i]) { slots[i].backup = (int)b; break; }
    }
  }

  // AFC's current_state is a closed enum (AFC.py State): Initialized, Idle,
  // Error, Loading, Unloading, ToolSwap, ToolDock, ToolPickup, Ejecting,
  // Moving, Restoring. Everything except the three resting states means the
  // unit is mid-operation -- matching the enum also catches Restoring, which
  // moves the toolhead.
  busy = !status_text.empty() && status_text != "Idle" &&
         status_text != "Initialized" && status_text != "Error";
}

void AfcBackend::load(int slot) {
  ws.gcode_script(fmt::format("TOOL_LOAD LANE={}", lane_id(slot)));
}

void AfcBackend::change_tool(int slot) {
  ws.gcode_script(fmt::format("CHANGE_TOOL LANE={}", lane_id(slot)));
}

void AfcBackend::unload() {
  ws.gcode_script("TOOL_UNLOAD");
}

void AfcBackend::eject(int slot) {
  ws.gcode_script(fmt::format("LANE_UNLOAD LANE={}", lane_id(slot)));
}

void AfcBackend::set_colour(int slot, const std::string &hex) {
  if (hex.empty()) {
    ws.gcode_script(fmt::format("SET_COLOR LANE={} COLOR=", lane_id(slot)));
  } else {
    ws.gcode_script(fmt::format("SET_COLOR LANE={} COLOR={}", lane_id(slot), hex));
  }
}

void AfcBackend::set_material(int slot, const std::string &material) {
  ws.gcode_script(fmt::format("SET_MATERIAL LANE={} MATERIAL={}", lane_id(slot), material));
}

void AfcBackend::set_backup(int slot, int backup) {
  if (backup < 0) {
    ws.gcode_script(fmt::format("SET_RUNOUT LANE={} RUNOUT=NONE", lane_id(slot)));
  } else {
    ws.gcode_script(fmt::format("SET_RUNOUT LANE={} RUNOUT={}", lane_id(slot), lane_id(backup)));
  }
}

void AfcBackend::reset_failure() {
  ws.gcode_script("RESET_FAILURE");
}

// AFC exposes message[0] of a queue it only pops on request, so an
// unacknowledged message hides every one behind it.
void AfcBackend::dismiss_message() {
  ws.gcode_script("AFC_CLEAR_MESSAGE");
}
