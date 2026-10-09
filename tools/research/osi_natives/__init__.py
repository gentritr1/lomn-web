"""M2a-2: the Python oracle's native models (tools/research/osi_vm.py). `framework` holds the host side every native
model uses (game objects, HostContext, the group registry, the VFS and the block files); the native groups of Tasks
7b/8b/9b register themselves with `framework.register_group`. Written from docs/osi/natives.md, never from engine/."""

# The native groups register themselves on import (framework.register_group); bind_natives runs them in this order.
from . import services  # noqa: E402,F401  (M2a-2 T7b: services and time)
from . import directors  # noqa: E402,F401  (M2a-2 T8b: directors, saver and the Beta 1.5.0 mods)
