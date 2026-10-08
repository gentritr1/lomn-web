.function main 0
    BranchAlways @mid
back:
    PushConstanti8 5
    Return
mid:
    BranchAlways @back
