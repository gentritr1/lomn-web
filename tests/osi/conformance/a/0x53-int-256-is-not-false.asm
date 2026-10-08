.function main 0
    PushConstanti16 256
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
