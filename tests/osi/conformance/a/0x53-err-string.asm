.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantString "x"
fail:
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
