.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti32 @sub
    JumpRelative 0
ret:
    Return
sub:
    PushConstantf32 1.0
fail:
    CompareAndBranchIfFalse @ret
