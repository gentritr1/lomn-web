; t-logic: the tag through And, Or, Not, BitwiseAnd, BitwiseOr, BitwiseXor, ShiftRight
.function main 0
    CallGameFunction t.src 0
    PushConstanti8 1
    And
    Not
    PushConstanti8 6
    BitwiseAnd
    PushConstanti8 3
    BitwiseOr
    PushConstanti8 1
    BitwiseXor
    PushConstanti8 1
    ShiftRight
b_logic:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 0
    CallGameFunction t.src 0
    Or
b_or:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    PushConstanti8 0
    Or
clean:
    CompareAndBranchIfFalse @c
c:
    PushConstanti8 1
    Return
