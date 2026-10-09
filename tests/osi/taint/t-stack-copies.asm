; t-stack-copies: the tag through 0x32 Swap, 0x33 Pull, 0x34 DupN, 0x35 Dup; the clean value beside it stays clean
.function main 0
    CallGameFunction t.src 0
    PushConstanti8 1
    Swap
    Dup
    Pull 3
clean:
    CompareAndBranchIfFalse @c1
c1:
    Pull 2
b_pull:
    CompareAndBranchIfFalse @c2
c2:
    DupN 2
b_dupn:
    CompareAndBranchIfFalse @c3
c3:
    PopN 2
b_swap:
    CompareAndBranchIfFalse @c4
c4:
    Return
