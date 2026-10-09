; t-type-only: a branch whose tagged input only went through `== nothing` (0x60 with a nothing operand), Not or a type
; test depends on the TYPE of the native's value, not on the value: the site and the native are marked type-only. A
; site that also reads a value is not; a native whose value reaches any sink is not
.function main 0
    CallGameFunction t.ty 0
    PushNothing
    EqualTo
    Not
b_ne:
    CompareAndBranchIfFalse @a
a:
    PushNothing
    CallGameFunction t.ty 0
    EqualTo
    CallGameFunction t.val 0
    Or
b_mixed:
    CompareAndBranchIfFalse @b
b:
    CallGameFunction t.src 0
    PushNothing
    EqualTo
    CallGameFunction t.src 0
    Or
b_both:
    CompareAndBranchIfFalse @c
c:
    CallGameFunction t.src 0
    IsInteger
b_test:
    CompareAndBranchIfFalse @d
d:
    PushConstanti8 1
    Return
