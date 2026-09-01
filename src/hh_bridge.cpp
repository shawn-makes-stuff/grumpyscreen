#include "hh_bridge.h"
#include "afc_panel.h"
#include "state.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

HappyHareBridge *HappyHareBridge::instance = NULL;

// Happy Hare numbers gates from 0; the panel's AFC contract names slots
// "lane1..laneN". gate g <-> "lane{g+1}", tool labels come from the TTG map.
std::string HappyHareBridge::lane_name(int gate) {
  return fmt::format("lane{}", gate + 1);
}

int HappyHareBridge::gate_of(const std::string &lane) {
  if (lane.rfind("lane", 0) != 0) return -1;
  try {
    return std::stoi(lane.substr(4)) - 1;
  } catch (const std::exception &) {
    return -1;
  }
}

// KEY=VALUE tokens; values may be quoted (the panel sends COLOR="")
static std::map<std::string, std::string> parse_params(std::istringstream &ss) {
  std::map<std::string, std::string> out;
  std::string tok;
  while (ss >> tok) {
    auto eq = tok.find('=');
    if (eq == std::string::npos) continue;
    std::string key = tok.substr(0, eq);
    std::string val = tok.substr(eq + 1);
    std::transform(key.begin(), key.end(), key.begin(), ::toupper);
    while (!val.empty() && (val.front() == '"' || val.front() == '\'')) val.erase(0, 1);
    while (!val.empty() && (val.back() == '"' || val.back() == '\'')) val.pop_back();
    out[key] = val;
  }
  return out;
}

// AfcPanel infers "busy" from keywords in current_state (load, tool, cut,
// moving, ...), so pick status text that reads naturally AND trips it
static std::string display_action(const std::string &action) {
  if (action == "Idle") return "Idle";
  if (action == "Loading") return "Loading";
  if (action == "Unloading") return "Unloading";
  if (action == "Loading Ext") return "Loading Extruder";
  if (action == "Exiting Ext") return "Unloading Extruder";
  if (action == "Forming Tip") return "Forming Tip / Cutting";
  if (action == "Heating") return "Heating Toolhead";
  if (action == "Checking") return "Checking Tools";
  if (action == "Homing") return "Homing / Moving";
  if (action == "Selecting") return "Selecting Tool";
  return action + " / Moving"; // unknown HH action: assume the unit is busy
}

// "RRGGBB" from HH's per-gate color. gate_color may be a w3c color name
// ("indigo"), so prefer the pre-resolved gate_color_rgb float tuple; an
// unset color stays "" (its rgb tuple would read as black).
static std::string gate_hex_color(const json &gate_color, const json &gate_color_rgb, int g) {
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

HappyHareBridge::HappyHareBridge(KWebSocketClient &c, std::mutex &l, AfcPanel &p)
  : NotifyConsumer(l)
  , ws(c)
  , panel(p)
  , active(false)
{
  instance = this;
  ws.register_notify_update(this);
}

HappyHareBridge::~HappyHareBridge() {
  instance = NULL;
}

bool HappyHareBridge::activate_if_detected() {
  State *state = State::get_instance();
  json &objs = state->get_data("/printer_objs/objects"_json_pointer);
  bool has_mmu = false;
  bool has_afc = false;
  if (objs.is_array()) {
    for (auto &o : objs) {
      const std::string name = o.template get<std::string>();
      if (name == "mmu") has_mmu = true;
      if (name == "AFC") has_afc = true;
    }
  }

  // a native AFC install always wins; the bridge only fills the gap
  const bool detected = has_mmu && !has_afc;
  if (detected && !active) {
    LOG_INFO("Happy Hare detected; bridging mmu state to the MMU panel");
    ws.set_gcode_rewriter([this](const std::string &g) { return rewrite_gcode(g); });
  } else if (!detected && active) {
    ws.set_gcode_rewriter(nullptr); // klipper reconfig removed Happy Hare
  }
  active = detected;
  pending_groups.clear();
  return active;
}

void HappyHareBridge::init_state() {
  if (active) {
    synthesize();
  }
}

void HappyHareBridge::consume(json &j) {
  if (!active) return;

  auto &status = j["/params/0"_json_pointer];
  if (!status.is_object() || !status.contains("mmu")) return;

  // State is registered first, so this delta is already merged
  if (status["mmu"].is_object() && status["mmu"].contains("endless_spool_groups")) {
    pending_groups.clear(); // klipper echoed back authoritative groups
  }
  synthesize();
  poke_panel();
}

// wake the panel with an AFC-shaped notify; it re-reads State
void HappyHareBridge::poke_panel() {
  json poke;
  poke["params"][0]["AFC"] = json::object();
  panel.consume(poke);
}

// HH publishes gate_spool_id but never grams; pull remaining weights from
// spoolman through moonraker (same proxy SpoolmanPanel uses)
void HappyHareBridge::fetch_spoolman_weights() {
  json params = {
    { "request_method", "GET" },
    { "path", "/v1/spool?allow_archived=true" },
  };
  ws.send_jsonrpc("server.spoolman.proxy", params, [this](json &d) {
    auto &spools = d["/result"_json_pointer];
    if (!spools.is_array()) {
      fetched_spool_ids.clear(); // failed; let the next synthesize retry
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
    synthesize(); // ids unchanged, so this can't re-fetch
    poke_panel();
  });
}

// =========================================================================
// STATE IN: /printer_state/mmu -> AFC-shaped objects the panel reads
// =========================================================================
void HappyHareBridge::synthesize() {
  State *state = State::get_instance();
  json &mmu = state->get_data("/printer_state/mmu"_json_pointer);
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
  const bool spoolman = mmu.value("spoolman_support", std::string("off")) != "off";

  bool es_enabled = false;
  if (mmu.contains("endless_spool_enabled")) {
    const json &e = mmu["endless_spool_enabled"];
    es_enabled = (e.is_boolean() && e.template get<bool>()) ||
                 (e.is_number() && e.template get<int>() != 0);
  }

  // endless spool groups -> cyclic runout chains, so the panel's backup
  // marks and its clear-backup flow behave with pair groups
  std::vector<std::string> runout(num_gates);
  if (es_enabled && es_groups.is_array() && (int)es_groups.size() >= num_gates) {
    for (int g = 0; g < num_gates; g++) {
      if (!es_groups[g].is_number()) continue;
      const int grp = es_groups[g].template get<int>();
      for (int step = 1; step < num_gates; step++) {
        const int other = (g + step) % num_gates;
        if (es_groups[other].is_number() && es_groups[other].template get<int>() == grp) {
          runout[g] = lane_name(other);
          break;
        }
      }
    }
  }

  auto tools_for_gate = [&](int g) {
    std::string s;
    if (ttg.is_array()) {
      for (size_t t = 0; t < ttg.size(); t++) {
        if (ttg[t].is_number() && ttg[t].template get<int>() == g) {
          if (!s.empty()) s += ",";
          s += fmt::format("T{}", t);
        }
      }
    }
    return s;
  };

  json &ps = state->get_data("/printer_state"_json_pointer);
  json lane_names = json::array();

  for (int g = 0; g < num_gates; g++) {
    const std::string name = lane_name(g);
    lane_names.push_back(name);

    int status = -1;
    if (gate_status.is_array() && g < (int)gate_status.size() && gate_status[g].is_number()) {
      status = gate_status[g].template get<int>();
    }
    const bool available = status > 0; // 1 = spool, 2 = from buffer

    json lane;
    lane["map"] = tools_for_gate(g);
    lane["material"] = (gate_material.is_array() && g < (int)gate_material.size() &&
                        gate_material[g].is_string())
                       ? gate_material[g].template get<std::string>() : "";
    lane["color"] = gate_hex_color(gate_color, gate_color_rgb, g);
    lane["prep"] = available;
    lane["load"] = available;
    lane["tool_loaded"] = (g == cur_gate) && tool_loaded;
    lane["loaded_to_hub"] = false;
    lane["runout_lane"] = runout[g].empty() ? json() : json(runout[g]);

    int spool_id = -1;
    if (gate_spool_id.is_array() && g < (int)gate_spool_id.size() &&
        gate_spool_id[g].is_number()) {
      spool_id = gate_spool_id[g].template get<int>();
    }
    lane["spool_id"] = spool_id > 0 ? json(spool_id) : json();
    // grams come from the spoolman fetch below, not from HH itself
    const auto w = spool_weights.find(spool_id);
    lane["weight"] = w != spool_weights.end() ? w->second : 0;

    ps[fmt::format("AFC_lane {}", name)] = lane;
  }

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

  const bool error = print_state == "error" || print_state == "pause_locked";

  json afc;
  afc["lanes"] = lane_names;
  afc["current_load"] = (tool_loaded && cur_gate >= 0) ? lane_name(cur_gate) : "";
  afc["current_state"] = display_action(action);
  afc["error_state"] = error;
  afc["bypass_state"] = cur_tool == -2;
  afc["spoolman"] = spoolman;
  afc["message"]["message"] = error ? (print_state == "error" ? "MMU error" : "MMU paused") : "";
  afc["message"]["type"] = "";
  ps["AFC"] = afc;

  // the panel discovers lanes through /printer_objs/objects; list the
  // synthesized names there (idempotent, lanes count is small)
  json &objs = state->get_data("/printer_objs/objects"_json_pointer);
  if (objs.is_array()) {
    for (int g = 0; g < num_gates; g++) {
      const std::string obj_name = fmt::format("AFC_lane {}", lane_name(g));
      const bool found = std::any_of(objs.begin(), objs.end(), [&](const json &o) {
        return o.is_string() && o.template get<std::string>() == obj_name;
      });
      if (!found) objs.push_back(obj_name);
    }
  }
}

// =========================================================================
// GCODE OUT: the panel's AFC commands -> MMU_* commands
// =========================================================================
std::string HappyHareBridge::rewrite_gcode(const std::string &gcode) {
  std::istringstream ss(gcode);
  std::string cmd;
  ss >> cmd;
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

  State *state = State::get_instance();

  if (cmd == "TOOL_UNLOAD") {
    return "MMU_UNLOAD";
  }
  if (cmd == "RESET_FAILURE") {
    // pause_locked wants the wakeup path; a hard error wants recovery
    const json psj = state->get_data("/printer_state/mmu/print_state"_json_pointer);
    const bool locked = psj.is_string() && psj.template get<std::string>() == "pause_locked";
    return locked ? "MMU_UNLOCK" : "MMU_RECOVER";
  }

  // everything below needs a LANE; any other command passes through untouched
  if (cmd != "TOOL_LOAD" && cmd != "CHANGE_TOOL" && cmd != "LANE_UNLOAD" &&
      cmd != "SET_COLOR" && cmd != "SET_MATERIAL" && cmd != "SET_RUNOUT") {
    return gcode;
  }

  auto params = parse_params(ss);
  const auto lane_it = params.find("LANE");
  const int g = lane_it == params.end() ? -1 : gate_of(lane_it->second);
  if (g < 0) return gcode;

  if (cmd == "TOOL_LOAD" || cmd == "CHANGE_TOOL") {
    // prefer the mapped tool so HH runs its full toolchange sequence
    const json ttg = state->get_data("/printer_state/mmu/ttg_map"_json_pointer);
    if (ttg.is_array()) {
      for (size_t t = 0; t < ttg.size(); t++) {
        if (ttg[t].is_number() && ttg[t].template get<int>() == g) {
          return fmt::format("MMU_CHANGE_TOOL TOOL={}", t);
        }
      }
    }
    return fmt::format("MMU_SELECT GATE={}\nMMU_LOAD", g);
  }
  if (cmd == "LANE_UNLOAD") {
    return fmt::format("MMU_EJECT GATE={}", g);
  }
  if (cmd == "SET_COLOR") {
    std::string color = params.count("COLOR") ? params["COLOR"] : "";
    if (!color.empty() && color[0] == '#') color = color.substr(1);
    return fmt::format("MMU_GATE_MAP GATE={} COLOR={}", g, color);
  }
  if (cmd == "SET_MATERIAL") {
    return fmt::format("MMU_GATE_MAP GATE={} MATERIAL={}", g, params["MATERIAL"]);
  }
  if (cmd == "SET_RUNOUT") {
    // the panel's pairwise runout -> endless spool groups. pending_groups
    // bridges the back-to-back clear-then-set commands the panel emits
    // before klipper echoes the first change back.
    std::vector<int> groups = pending_groups;
    if (groups.empty()) {
      const json es = state->get_data("/printer_state/mmu/endless_spool_groups"_json_pointer);
      const json ng = state->get_data("/printer_state/mmu/num_gates"_json_pointer);
      const int n = ng.is_number() ? ng.template get<int>() : 0;
      for (int i = 0; i < n; i++) {
        groups.push_back(es.is_array() && i < (int)es.size() && es[i].is_number()
                         ? es[i].template get<int>() : i);
      }
    }
    if ((int)groups.size() <= g) return gcode;

    std::string runout = params.count("RUNOUT") ? params["RUNOUT"] : "NONE";
    std::transform(runout.begin(), runout.end(), runout.begin(), ::toupper);
    if (runout.empty() || runout == "NONE") {
      // leave the old group; former partners keep each other
      groups[g] = *std::max_element(groups.begin(), groups.end()) + 1;
    } else {
      std::string target = params["RUNOUT"];
      const int backup = gate_of(target);
      if (backup < 0 || backup >= (int)groups.size()) return gcode;
      groups[backup] = groups[g];
    }
    pending_groups = groups;

    std::string csv;
    for (size_t i = 0; i < groups.size(); i++) {
      if (i) csv += ",";
      csv += std::to_string(groups[i]);
    }
    return fmt::format("MMU_ENDLESS_SPOOL GROUPS={}", csv);
  }

  return gcode;
}
