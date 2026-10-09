; t-sink-tier1-arg: a tagged argument of a tier1 native is a sink (natives.md §7.3 item 3); a clean one is not
.function main 0
    CallGameFunction t.src 0
c_tainted:
    CallGameFunction t.impl 1
    Pop
    PushConstanti8 1
    CallGameFunction t.impl 1
    Pop
    PushConstanti8 1
    Return
