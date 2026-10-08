.function main 0
    PushNothing
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
