// M2a-2 T3 fix 1 (review I-1): the framework's link probe. It binds `t3selftest.ping` only when the registry has that row,
// which no data/osi table has: in a real run it binds nothing, so it never counts as a group that binds a body. Its test
// (test_osi_natives.cpp, "the explicit group list reaches the link") gives the row and checks the body ran, through the
// lomn_osi library target, natively and as wasm32.
#include "lomn/osi/natives.h"

namespace lomn::osi {

namespace {
Variant selftest_ping(NativeCall&, void*) { return make_int(0x5e1f7e57); }
}  // namespace

void bind_selftest_group(NativeRegistry& reg, HostContext& ctx) {
  if (reg.find("t3selftest", "ping")) reg.bind("t3selftest.ping", selftest_ping, &ctx, Tier::Implemented);
}

}  // namespace lomn::osi
