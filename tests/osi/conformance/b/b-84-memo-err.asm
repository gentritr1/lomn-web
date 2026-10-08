; b-84-memo-err
.function main2 0 @m2
.function main 0
    CallGameFunction t.timer 0
    PushConstanti32 @f
    JumpRelative 1
    Pop
    PushConstanti8 3
    PushConstanti32 @f
    JumpRelative 1
    Return
f:
err:
    CallGameFunction t.usetimer 1
    Return
m2:
    PushConstant0
    Return
