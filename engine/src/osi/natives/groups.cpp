// M2a-2 T3 fix 1 (review I-1): the explicit list of native groups linked into lomn_osi. A group is a translation unit of
// engine/src/osi/natives/ with one BindFn; it is listed here, so bind_natives() references it and the linker always pulls
// it out of the STATIC archive (a self-registering static initialiser would be dropped: nothing references its object
// file). Tasks 7a/8a/9a each add one declaration and one row.
#include "lomn/osi/natives.h"

namespace lomn::osi {

void bind_selftest_group(NativeRegistry& reg, HostContext& ctx);  // natives/selftest.cpp
void bind_services_group(NativeRegistry& reg, HostContext& ctx);  // natives/services.cpp (M2a-2 T7a)
void bind_objects_group(NativeRegistry& reg, HostContext& ctx);   // natives/objects.cpp (M2a-2 T9a)

const std::vector<NativeGroup>& native_groups() {
  static const std::vector<NativeGroup> g = {
      {"selftest", bind_selftest_group, true},
      {"services", bind_services_group, false},  // M2a-2 T7a: strings, file-exists, GcTimer.create, time, resolution
      {"objects", bind_objects_group, false},    // M2a-2 T9a: objects, model director, camera, HUD
  };
  return g;
}

}  // namespace lomn::osi
