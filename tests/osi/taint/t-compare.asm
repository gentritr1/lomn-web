; t-compare: 0x60 EqualTo and 0x61 LessThan on a tagged operand (either side); a clean compare stays clean
.function main 0
    CallGameFunction t.src 0
    PushConstant0
    EqualTo
b_eq:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 5
    CallGameFunction t.src 0
    LessThan
b_lt:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    PushConstanti8 2
    GreaterOrEqual
clean:
    CompareAndBranchIfFalse @c
c:
    PushConstanti8 1
    Return
