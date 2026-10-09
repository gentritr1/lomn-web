; t-script-call: a tagged argument of a 0x50 script call reaches the callee's frame and comes back through 0x52
.function main 0
    CallGameFunction t.src 0
    PushConstanti8 1
    PushConstanti32 @sub
    JumpRelative 2
b_ret:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 0
    CallGameFunction t.src 0
    PushConstanti32 @sub
    JumpRelative 2
clean:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    Return
sub:
    GetVariableValue 0
    Return
