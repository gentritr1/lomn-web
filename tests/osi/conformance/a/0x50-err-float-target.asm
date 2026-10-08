.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti8 5
    PushConstantf32 2.5
fail:
    JumpRelative 0
    Return
