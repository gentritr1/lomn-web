.function main2 0
    PushConstanti8 1
    Return
.function main 0
    CreateStackVariables 3
    PushConstantf32 1.0
fail:
    CompareAndBranchIfFalse @end
end:
    Return
