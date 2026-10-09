; t-arith: the union of the operands through + - * / % << ~ ^ and 0x7a Increment
.function main 0
    CallGameFunction t.src 0
    PushConstanti8 1
    Add
    Increment
    PushConstanti8 2
    Multiply
    PushConstanti8 1
    Subtract
    PushConstanti8 1
    ShiftLeft
    BitwiseNot
    PushConstanti8 3
    Modulus
b_arith:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 8
    CallGameFunction t.src 0
    PushConstanti8 2
    Add
d_rhs:
    Divide
b_rhs:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 4
    PushConstanti8 2
    Divide
clean:
    CompareAndBranchIfFalse @c
c:
    CallGameFunction t.src 0
    PushConstanti8 2
    Power
b_pow:
    CompareAndBranchIfFalse @d
d:
    PushConstanti8 1
    Return
