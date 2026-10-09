; t-neg-pop-only (negative): tagged values that are only popped reach no sink; the natives are safe
.function main 0
    CallGameFunction t.src 0
    Pop
    CallGameFunction t.src 0
    CallGameFunction t.str 0
    PopN 2
    PushConstanti8 1
clean:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 1
    Return
