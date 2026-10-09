; t-compare: 0x60-0x64 on a tagged operand (either side); a clean compare stays clean
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
    CallGameFunction t.src 0
    PushConstanti8 1
    GreaterThan
b_gt:
    CompareAndBranchIfFalse @d
d:
    CallGameFunction t.src 0
    PushConstanti8 1
    LessOrEqual
b_le:
    CompareAndBranchIfFalse @f
f:
    PushConstanti8 2
    CallGameFunction t.src 0
    GreaterOrEqual
b_ge:
    CompareAndBranchIfFalse @g
g:
    PushConstanti8 1
    Return
