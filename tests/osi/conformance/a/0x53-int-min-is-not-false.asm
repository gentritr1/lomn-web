.function main 0
    PushConstanti32 -2147483648
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
