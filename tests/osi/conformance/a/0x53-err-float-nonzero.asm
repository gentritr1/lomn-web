.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantf32 1.0
fail:
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
