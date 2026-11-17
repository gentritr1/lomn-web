// M2a-3 T6a: the HUD object state the menu frame renders (hud.md §4) and its `Z hud/sprite/text/font` lines (hud.md
// §9.2). The state is kept per game handle as the printed value of each field, so an object a conformance test native
// minted (no host object) is known too, with every field `-` until a setter writes it. Citations: natives/hud.cpp.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lomn/osi/natives.h"

namespace lomn::osi {

struct HudModel {
  explicit HudModel(HostContext&) {}
  uint16_t sprite_type = 0, text_type = 0, font_type = 0;  // the creators' declared returns (bind_hud_group)
  std::map<uint32_t, std::map<std::string, std::string>> fields;  // handle -> field -> printed value (unset: `-`)
  std::vector<uint32_t> shown;  // the shown node ids in draw order (mainHud's child vector, hud.md §3.1)
};

// The constructors' fields (called by the creators in objects.cpp once the handle is minted).
void hud_created_sprite(HostContext& ctx, uint32_t handle, const uint32_t rect[4], const std::string& file, uint8_t b6);
void hud_created_text(HostContext& ctx, uint32_t handle, const uint32_t pos[3], GameObject* font, const std::string& s);
void hud_created_font(HostContext& ctx, uint32_t handle, const std::string& file);

// The `Z hud.node`, `Z sprite.*`, `Z text.*`, `Z font.*` lines (unsorted; snapshot_lines sorts the block).
std::vector<std::string> hud_snapshot_lines(HostContext& ctx);

}  // namespace lomn::osi
