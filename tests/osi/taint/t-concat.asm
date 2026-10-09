; t-concat: string + string keeps the union of both sides (0x70 on 0x16/0x11 operands)
.function main 0
    CallGameFunction t.str 0
    PushConstantString "x"
    Add
    PushConstantString "x"
    EqualTo
b_lhs:
    CompareAndBranchIfFalse @a
a:
    PushConstantString "y"
    CallGameFunction t.str 0
    Add
    PushConstantString "y"
    EqualTo
b_rhs:
    CompareAndBranchIfFalse @b
b:
    PushConstantString "a"
    PushConstantString "b"
    Add
    PushConstantString "ab"
    EqualTo
clean:
    CompareAndBranchIfFalse @c
c:
    PushConstanti8 1
    Return
