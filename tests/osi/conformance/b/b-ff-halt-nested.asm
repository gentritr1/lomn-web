; b-ff-halt-nested
.function main 0
    PushConstanti32 @f
    JumpRelative 0
    PushConstanti8 9
    Return
f:
    PushConstanti8 4
    Halt
