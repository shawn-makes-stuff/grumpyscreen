#include "hh_backend.h"
#include "state.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>

// Neutral status-bar text for HH's action states
static std::string display_action(const std::string &action) {
  if (action == "Loading Ext") return "Loading Extruder";
  if (action == "Exiting Ext") return "Unloading Extruder";
  if (action == "Forming Tip") return "Forming Tip";
  if (action == "Heating") return "Heating Toolhead";
  if (action == "Checking") return "Checking Tools";
  if (action == "Homing") return "Homing";
  if (action == "Selecting") return "Selecting Tool";
  return action; // Idle, Loading, Unloading, ... already read fine
}

// "RRGGBB" from HH's per-gate colour. gate_color may be a w3c colour name
// ("indigo"), so prefer the pre-resolved gate_color_rgb float tuple; an
// unset colour stays "" (its rgb tuple would read as black).
static std::string gate_hex_colour(const json &gate_color, const json &gate_color_rgb, int g) {
  std::string name;
  if (gate_color.is_array() && g < (int)gate_color.size() && gate_color[g].is_string()) {
    name = gate_color[g].template get<std::string>();
  }
  if (name.empty()) return "";

  if (gate_color_rgb.is_array() && g < (int)gate_color_rgb.size() &&
      gate_color_rgb[g].is_array() && gate_color_rgb[g].size() == 3) {
    int rgb[3];
    for (int i = 0; i < 3; i++) {
      double v = gate_color_rgb[g][i].is_number() ? gate_color_rgb[g][i].template get<double>() : 0.0;
      rgb[i] = std::max(0, std::min(255, (int)std::lround(v * 255.0)));
    }
    return fmt::format("{:02X}{:02X}{:02X}", rgb[0], rgb[1], rgb[2]);
  }

  if (!name.empty() && name[0] == '#') name = name.substr(1);
  if (name.size() == 6 &&
      std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); })) {
    return name;
  }
  return "";
}

bool HhBackend::detect() {
  json &objs = State::get_instance()->get_data("/printer_objs/objects"_json_pointer);
  if (!objs.is_array()) return false;
  bool found = std::any_of(objs.begin(), objs.end(), [](const json &o) {
    return o.is_string() && o.template get<std::string>() == "mmu";
  });
  if (found) pending_groups.clear();
  return found;
}

bool HhBackend::owns_update(json &j) {
  auto &status = j["/params/0"_json_pointer];
  if (!status.is_object() || !status.contains("mmu")) return false;
  if (status["mmu"].is_object() && status["mmu"].contains("endless_spool_groups")) {
    pending_groups.clear(); // klipper echoed back authoritative groups
  }
  return true;
}

void HhBackend::refresh() {
  State *state = State::get_instance();
  json &mmu = state->get_data("/printer_state/mmu"_json_pointer);

  slots.clear();
  loaded_slot = -1;
  status_text = "";
  message = "";
  error = false;
  bypass = false;
  busy = false;

  if (!mmu.is_object()) return;

  const int num_gates = mmu.value("num_gates", 0);
  if (num_gates <= 0) return;

  const json ttg = mmu.value("ttg_map", json::array());
  const json gate_status = mmu.value("gate_status", json::array());
  const json gate_material = mmu.value("gate_material", json::array());
  const json gate_color = mmu.value("gate_color", json::array());
  const json gate_color_rgb = mmu.value("gate_color_rgb", json::array());
  const json gate_spool_id = mmu.value("gate_spool_id", json::array());
  const json es_groups = mmu.value("endless_spool_groups", json::array());

  const int cur_gate = mmu.value("gate", -1);
  const int cur_tool = mmu.value("tool", -1);
  const bool tool_loaded = mmu.value("filament", std::string()) == "Loaded";
  const std::string action = mmu.value("action", std::string("Idle"));
  const std::string print_state = mmu.value("print_state", std::string());
  const std::string spoolman_mode = mmu.value("spoolman_support", std::string("off"));
  spoolman = spoolman_mode != "off";
  // in pull mode spoolman owns the gate map and Happy Hare refuses local edits
  const bool editable = spoolman_mode != "pull";

  bool es_enabled = false;
  if (mmu.contains("endless_spool_enabled")) {
    const json &e = mmu["endless_spool_enabled"];
    es_enabled = (e.is_boolean() && e.template get<bool>()) ||
                 (e.is_number() && e.template get<int>() != 0);
  }

  for (int g = 0; g < num_gates; g++) {
    MmuSlot slot;
    slot.name = fmt::format("Gate {}", g);

    if (ttg.is_array()) {
      for (size_t t = 0; t < ttg.size(); t++) {
        if (ttg[t].is_number() && ttg[t].template get<int>() == g) {
          if (!slot.map.empty()) slot.map += ",";
          slot.map += fmt::format("T{}", t);
        }
      }
    }

    if (gate_material.is_array() && g < (int)gate_material.size() &&
        gate_material[g].is_string()) {
      slot.material = gate_material[g].template get<std::string>();
    }
    slot.colour = gate_hex_colour(gate_color, gate_color_rgb, g);

    int status = -1;
    if (gate_status.is_array() && g < (int)gate_status.size() && gate_status[g].is_number()) {
      status = gate_status[g].template get<int>();
    }
    const bool available = status > 0; // 1 = spool, 2 = from buffer
    slot.prepped = available;
    slot.ready = available;
    slot.tool_loaded = (g == cur_gate) && tool_loaded;
    slot.can_configure = editable;

    if (gate_spool_id.is_array() && g < (int)gate_spool_id.size() &&
        gate_spool_id[g].is_number()) {
      const int sid = gate_spool_id[g].template get<int>();
      if (sid > 0) {
        slot.spool_id = sid;
        const auto w = spool_weights.find(sid);
        if (w != spool_weights.end()) slot.weight = w->second;
      }
    }

    slots.push_back(slot);
  }

  // endless spool groups -> per-slot backup: the next gate in the same group
  if (es_enabled && es_groups.is_array() && (int)es_groups.size() >= num_gates) {
    for (int g = 0; g < num_gates; g++) {
      if (!es_groups[g].is_number()) continue;
      const int grp = es_groups[g].template get<int>();
      for (int step = 1; step < num_gates; step++) {
        const int other = (g + step) % num_gates;
        if (es_groups[other].is_number() && es_groups[other].template get<int>() == grp) {
          slots[g].backup = other;
          break;
        }
      }
    }
  }

  if (tool_loaded && cur_gate >= 0 && cur_gate < num_gates) loaded_slot = cur_gate;

  error = print_state == "error" || print_state == "pause_locked";
  message = error ? (print_state == "error" ? "MMU error" : "MMU paused") : "";
  bypass = cur_tool == -2;
  busy = action != "Idle";
  status_text = display_action(action);

  // refresh spoolman weights whenever the set of assigned spool ids changes
  if (spoolman) {
    std::vector<int> ids;
    if (gate_spool_id.is_array()) {
      for (auto &sid : gate_spool_id) {
        if (sid.is_number() && sid.template get<int>() > 0) {
          ids.push_back(sid.template get<int>());
        }
      }
    }
    if (!ids.empty() && ids != fetched_spool_ids) {
      fetched_spool_ids = ids;
      fetch_spoolman_weights();
    }
  }
}

void HhBackend::fetch_spoolman_weights() {
  json params = {
    { "request_method", "GET" },
    { "path", "/v1/spool?allow_archived=true" },
  };
  ws.send_jsonrpc("server.spoolman.proxy", params, [this](json &d) {
    auto &spools = d["/result"_json_pointer];
    if (!spools.is_array()) {
      fetched_spool_ids.clear(); // failed; let the next refresh retry
      return;
    }
    for (auto &s : spools) {
      if (s.contains("id") && s["id"].is_number()) {
        int grams = 0;
        if (s.contains("remaining_weight") && s["remaining_weight"].is_number()) {
          grams = (int)std::lround(s["remaining_weight"].template get<double>());
        }
        spool_weights[s["id"].template get<int>()] = grams;
      }
    }
    if (changed) changed(); // ids unchanged, so the re-refresh can't re-fetch
  });
}

void HhBackend::load(int slot) {
  change_tool(slot);
}

void HhBackend::change_tool(int slot) {
  // prefer the mapped tool so HH runs its full toolchange sequence
  if (!slots[slot].map.empty()) {
    json &mmu = State::get_instance()->get_data("/printer_state/mmu"_json_pointer);
    const json ttg = mmu.is_object() ? mmu.value("ttg_map", json::array()) : json::array();
    if (ttg.is_array()) {
      for (size_t t = 0; t < ttg.size(); t++) {
        if (ttg[t].is_number() && ttg[t].template get<int>() == slot) {
          ws.gcode_script(fmt::format("MMU_CHANGE_TOOL TOOL={}", t));
          return;
        }
      }
    }
  }
  ws.gcode_script(fmt::format("MMU_SELECT GATE={}\nMMU_LOAD", slot));
}

void HhBackend::unload() {
  ws.gcode_script("MMU_UNLOAD");
}

void HhBackend::eject(int slot) {
  ws.gcode_script(fmt::format("MMU_EJECT GATE={}", slot));
}

void HhBackend::set_colour(int slot, const std::string &hex) {
  ws.gcode_script(fmt::format("MMU_GATE_MAP GATE={} COLOR={}", slot, hex));
}

void HhBackend::set_material(int slot, const std::string &material) {
  ws.gcode_script(fmt::format("MMU_GATE_MAP GATE={} MATERIAL={}", slot, material));
}

std::vector<int> HhBackend::current_groups() const {
  if (!pending_groups.empty()) return pending_groups;
  State *state = State::get_instance();
  const json es = state->get_data("/printer_state/mmu/endless_spool_groups"_json_pointer);
  const json ng = state->get_data("/printer_state/mmu/num_gates"_json_pointer);
  const int n = ng.is_number() ? ng.template get<int>() : 0;
  std::vector<int> groups;
  for (int i = 0; i < n; i++) {
    groups.push_back(es.is_array() && i < (int)es.size() && es[i].is_number()
                     ? es[i].template get<int>() : i);
  }
  return groups;
}

void HhBackend::send_groups(const std::vector<int> &groups) {
  pending_groups = groups;
  std::string csv;
  for (size_t i = 0; i < groups.size(); i++) {
    if (i) csv += ",";
    csv += std::to_string(groups[i]);
  }
  ws.gcode_script(fmt::format("MMU_ENDLESS_SPOOL GROUPS={}", csv));
}

// the panel's pairwise backup -> endless spool groups
void HhBackend::set_backup(int slot, int backup) {
  std::vector<int> groups = current_groups();
  if ((int)groups.size() <= slot) return;

  if (backup < 0) {
    // leave the old group; former partners keep each other
    groups[slot] = *std::max_element(groups.begin(), groups.end()) + 1;
  } else {
    if (backup >= (int)groups.size()) return;
    groups[backup] = groups[slot];
  }
  send_groups(groups);
}

void HhBackend::reset_failure() {
  // pause_locked wants the wakeup path; a hard error wants recovery
  const json ps = State::get_instance()->get_data("/printer_state/mmu/print_state"_json_pointer);
  const bool locked = ps.is_string() && ps.template get<std::string>() == "pause_locked";
  ws.gcode_script(locked ? "MMU_UNLOCK" : "MMU_RECOVER");
}
