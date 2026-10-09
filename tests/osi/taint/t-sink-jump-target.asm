; t-sink-jump-target: a 0x50/0x51 target computed from a tagged value decides which code runs: a branch sink (an
; addition to item 1); a literal target is not
.function main 0
    CallGameFunction t.src 0
    PushConstanti32 @sub
    Add
j:
    JumpRelative 0
    Pop
    PushConstanti32 @sub
    JumpRelative 0
    Pop
    PushConstanti8 1
    Return
sub:
    PushConstanti8 1
    Return
