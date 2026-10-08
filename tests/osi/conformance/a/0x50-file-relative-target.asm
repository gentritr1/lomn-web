.function main 0
    PushConstanti32 @sub
    JumpRelative 0
    Return
early:
    PushConstanti8 99
    Return
    Nop
sub:
    PushConstanti8 7
    Return
    PushConstanti8 55
    Return
